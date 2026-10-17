/// M3-W4 客户端（rtdb_client）集成测试：请求面（AUTH/PING/DDL/EXEC_BATCH/
/// SCAN/SCAN_PAGE）、订阅泵（有序交付/断连续传/溢出重取/多订阅并发）与
/// C ABI 精简面（docs/13 §5-W4）。
///
/// 对端 = 进程内 `server::Server`（W3）；写入侧经 `srv.engine()` 直写，
/// 与订阅交付路径（WAL 环）天然一致。

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "rtdb/proto_v1.h"
#include "rtdb/record_block.hpp"
#include "rtdb/rtdb_client.hpp"
#include "rtdb/rtdb_client_c.h"
#include "server/config.hpp"
#include "server/server.hpp"

namespace fs = std::filesystem;

namespace {

using rtdb::Err;

constexpr const char* kSalt = "client-salt";
constexpr const char* kUser = "admin";
constexpr const char* kPassword = "pw-123";

rtdb::TableDef MakeDef3(const std::string& name) {
    rtdb::TableDef def;
    def.name = name;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
    def.columns.push_back({"msg", rtdb::DataType::kUtf8Var, 0, true, false});
    return def;
}

/// 撞环压力表（无变长列：var 池容量有限，不用于 1800 行级压测）。
rtdb::TableDef MakeDef2(const std::string& name) {
    rtdb::TableDef def;
    def.name = name;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
    return def;
}

/// 变更收集器（泵线程写、测试主线程读）。
struct Collector {
    std::mutex mu;
    std::vector<rtdb::ChangeEvent> events;
    std::vector<std::pair<bool, uint32_t>> states;  ///< (ready, attempt)
    std::atomic<int> overflows{0};
    std::atomic<uint64_t> rescanned{0};

    rtdb::SubscriptionOptions MakeOptions(std::vector<std::string> tables) {
        rtdb::SubscriptionOptions o;
        o.tables = std::move(tables);
        o.delta_wait_ms = 1000;
        o.reconnect_min_ms = 100;
        o.reconnect_max_ms = 500;
        o.on_change = [this](const rtdb::ChangeEvent& ev) {
            std::lock_guard<std::mutex> lk(mu);
            events.push_back(ev);
        };
        o.on_state = [this](bool ready, uint32_t attempt) {
            std::lock_guard<std::mutex> lk(mu);
            states.emplace_back(ready, attempt);
        };
        return o;
    }

    size_t Count() {
        std::lock_guard<std::mutex> lk(mu);
        return events.size();
    }

    std::vector<rtdb::ChangeEvent> Snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return events;
    }

    bool WaitCount(size_t n, uint32_t timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (Count() >= n) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return Count() >= n;
    }

    /// 等待主键 = id 的 insert 事件（溢出重同步后的最终一致性判据）。
    bool WaitRow(int64_t id, uint32_t timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu);
                for (const rtdb::ChangeEvent& ev : events) {
                    if (ev.kind == rtdb::ChangeEvent::Kind::kInsert && !ev.row.empty() &&
                        ev.row[0].as_int() == id) {
                        return true;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
};

class ClientTest : public ::testing::Test {
protected:
    rtdb::server::Server srv;
    rtdb::server::Config cfg;
    std::string path;
    uint16_t port = 0;

    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag = info->name();
        path = "rtdb_gtest_client_" + tag + ".rtdb";
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path + ".snap", ec);

        cfg.instance = "cli_" + tag;
        cfg.path = path;
        cfg.initial_size_mb = 32;
        cfg.listen_host = "127.0.0.1";
        cfg.listen_port = 0;  // 系统分配；port() 取回
        cfg.fsync = rtdb::FsyncPolicy::Never;
        cfg.auth_salt = kSalt;
        cfg.max_payload_bytes = 4u << 20;
        rtdb::server::UserEntry u;
        u.name = kUser;
        u.digest = rtdb::server::HashPassword(cfg.auth_salt, kPassword);
        cfg.users.push_back(u);

        ASSERT_TRUE(rtdb::IsOk(srv.Start(cfg))) << srv.last_error();
        port = srv.port();
        ASSERT_NE(port, 0);
    }

    void TearDown() override {
        srv.Stop();
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path + ".snap", ec);
    }

    rtdb::ClientOptions MakeOptions() const {
        rtdb::ClientOptions o;
        o.host = "127.0.0.1";
        o.port = port;
        o.user = kUser;
        o.password = kPassword;
        o.auth_salt = kSalt;
        o.connect_timeout_ms = 2000;
        o.request_timeout_ms = 3000;
        return o;
    }

    std::unique_ptr<rtdb::Client> Connect() {
        auto r = rtdb::Client::Connect(MakeOptions());
        EXPECT_TRUE(r.IsOk()) << "连接失败：" << static_cast<int>(r.Error());
        if (!r.IsOk()) return nullptr;
        return r.TakeValue();
    }

    /// 引擎直写（id = start..start+n-1，val = id*10，msg = "s<id>"）。
    void SeedViaEngine(const std::string& table, int n, int start = 1) {
        auto t = srv.engine()->OpenTable(table);
        ASSERT_TRUE(t.IsOk());
        for (int i = start; i < start + n; ++i) {
            const auto r = srv.engine()->Insert(
                t.Value(), {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i * 10),
                            rtdb::ColumnValue::Text("s" + std::to_string(i))});
            ASSERT_TRUE(r.IsOk()) << "Insert 失败：" << static_cast<int>(r.Error());
        }
    }

    /// 2 列直写（MakeDef2 表；val = id*10）。
    void SeedRow2(const std::string& table, int64_t id) {
        auto t = srv.engine()->OpenTable(table);
        ASSERT_TRUE(t.IsOk());
        const auto r =
            srv.engine()->Insert(t.Value(), {rtdb::ColumnValue::Int(id),
                                             rtdb::ColumnValue::Int(id * 10)});
        ASSERT_TRUE(r.IsOk()) << "Insert 失败：" << static_cast<int>(r.Error());
    }

    /// 写一批 n 行（撞环压力用；val = i）。
    void WriteBatch600(const std::string& table, int base) {
        auto t = srv.engine()->OpenTable(table);
        ASSERT_TRUE(t.IsOk());
        std::vector<rtdb::Engine::BatchOp> ops;
        ops.reserve(600);
        for (int i = 0; i < 600; ++i) {
            rtdb::Engine::BatchOp op;
            op.type = rtdb::Engine::BatchOp::Type::kInsert;
            op.table = t.Value();
            op.row = {rtdb::ColumnValue::Int(base + i), rtdb::ColumnValue::Int(i)};
            ops.push_back(std::move(op));
        }
        const auto r = srv.engine()->ExecuteBatch(ops);
        ASSERT_TRUE(r.IsOk()) << "ExecuteBatch 失败（base=" << base
                              << "）：" << static_cast<int>(r.Error());
    }
};

// ---------------- 连接 / 认证 / PING ----------------

TEST_F(ClientTest, ConnectAuthAndPing) {
    // 口令错 → 501（不建连接对象）
    {
        rtdb::ClientOptions bad = MakeOptions();
        bad.password = "wrong";
        auto r = rtdb::Client::Connect(bad);
        ASSERT_FALSE(r.IsOk());
        EXPECT_EQ(r.Error(), Err::kAuthFailed);
    }
    // 端口缺失 → 101
    {
        rtdb::ClientOptions bad = MakeOptions();
        bad.port = 0;
        EXPECT_EQ(rtdb::Client::Connect(bad).Error(), Err::kInvalidArgument);
    }

    auto c = Connect();
    ASSERT_TRUE(c != nullptr);
    EXPECT_TRUE(c->connected());
    EXPECT_NE(c->session(), 0u);

    auto p = c->Ping();
    ASSERT_TRUE(p.IsOk());
    EXPECT_GE(p.Value().committed_seq, 0);
    EXPECT_GT(p.Value().writer_heartbeat_ns, 0u);
    EXPECT_EQ(p.Value().session_ttl_ms, 30u * 60u * 1000u);

    // 主动断开 → 读类请求自动重连（PING 重发）
    c->Close();
    EXPECT_FALSE(c->connected());
    auto p2 = c->Ping();
    EXPECT_TRUE(p2.IsOk());
    EXPECT_TRUE(c->connected());
    c->Close();
    EXPECT_FALSE(c->connected());
}

// ---------------- DDL / 批 / 扫描回环 ----------------

TEST_F(ClientTest, DdlBatchScanRoundTrip) {
    auto c = Connect();
    ASSERT_TRUE(c != nullptr);

    rtdb::TableDef def = MakeDef3("t_api");
    def.max_rows_hint = 512;
    auto ct = c->CreateTable(def);
    ASSERT_TRUE(ct.IsOk()) << static_cast<int>(ct.Error());
    EXPECT_NE(ct.Value().table_id, 0);
    EXPECT_EQ(ct.Value().visible_columns, 3u);
    EXPECT_EQ(c->CreateTable(def).Error(), Err::kTableExists);

    // ops 批：插入 3 行
    std::vector<rtdb::ClientBatchOp> ops;
    for (int i = 1; i <= 3; ++i) {
        rtdb::ClientBatchOp op;
        op.type = rtdb::ClientBatchOp::Type::kInsert;
        op.table = "t_api";
        op.row = {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i * 10),
                  rtdb::ColumnValue::Text("s" + std::to_string(i))};
        ops.push_back(std::move(op));
    }
    auto ex = c->ExecuteBatch(ops);
    ASSERT_TRUE(ex.IsOk());
    EXPECT_GT(ex.Value(), 0);

    // 全表扫描 + 自描述列
    auto sc = c->Scan("t_api");
    ASSERT_TRUE(sc.IsOk());
    ASSERT_EQ(sc.Value().rows.size(), 3u);
    ASSERT_EQ(sc.Value().schema.col_names.size(), 3u);
    EXPECT_EQ(sc.Value().schema.name, "t_api");
    EXPECT_EQ(sc.Value().schema.col_types[2], rtdb::DataType::kUtf8Var);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(sc.Value().rows[i][0].as_int(), static_cast<int64_t>(i + 1));
        EXPECT_EQ(sc.Value().rows[i][1].as_int(), static_cast<int64_t>(i + 1) * 10);
        EXPECT_EQ(sc.Value().rows[i][2].as_bytes(), "s" + std::to_string(i + 1));
    }

    // 区间 [2,3]
    rtdb::Engine::KeyRange range;
    range.lo = rtdb::ColumnValue::Int(2);
    range.hi = rtdb::ColumnValue::Int(3);
    auto rng = c->Scan("t_api", range);
    ASSERT_TRUE(rng.IsOk());
    ASSERT_EQ(rng.Value().rows.size(), 2u);
    EXPECT_EQ(rng.Value().rows.front()[0].as_int(), 2);
    EXPECT_EQ(rng.Value().rows.back()[0].as_int(), 3);

    // 浮点边界 → 101（SCAN 边界只支持整数类）
    range.lo = rtdb::ColumnValue::Real(1.5);
    EXPECT_EQ(c->Scan("t_api", range).Error(), Err::kInvalidArgument);

    // update + delete（批内混用）
    std::vector<rtdb::ClientBatchOp> ops2;
    {
        rtdb::ClientBatchOp op;
        op.type = rtdb::ClientBatchOp::Type::kUpdate;
        op.table = "t_api";
        op.row = {rtdb::ColumnValue::Int(2), rtdb::ColumnValue::Int(99),
                  rtdb::ColumnValue::Text("u2")};
        ops2.push_back(std::move(op));
    }
    {
        rtdb::ClientBatchOp op;
        op.type = rtdb::ClientBatchOp::Type::kDelete;
        op.table = "t_api";
        op.row = {rtdb::ColumnValue::Int(1)};
        ops2.push_back(std::move(op));
    }
    ASSERT_TRUE(c->ExecuteBatch(ops2).IsOk());

    // 块 upsert：追加 id 4/5
    std::vector<int64_t> ids{4, 5};
    std::vector<int64_t> vals{40, 50};
    std::vector<std::string> msg_store{"s4", "s5"};
    std::vector<std::string_view> msgs{msg_store[0], msg_store[1]};
    rtdb::RecordBlock blk("t_api", 2);
    blk.AppendColumn("id", ids).AppendColumn("val", vals).AppendColumn("msg", msgs);
    auto ub = c->UpsertBatch(blk.View());
    ASSERT_TRUE(ub.IsOk());

    auto sc2 = c->Scan("t_api");
    ASSERT_TRUE(sc2.IsOk());
    ASSERT_EQ(sc2.Value().rows.size(), 4u);  // 2,3,4,5
    EXPECT_EQ(sc2.Value().rows[0][0].as_int(), 2);
    EXPECT_EQ(sc2.Value().rows[0][1].as_int(), 99);
    EXPECT_EQ(sc2.Value().rows[3][0].as_int(), 5);
    EXPECT_EQ(sc2.Value().rows[3][2].as_bytes(), "s5");

    // 描述 / 缺失表
    auto de = c->DescribeTable("t_api");
    ASSERT_TRUE(de.IsOk());
    EXPECT_EQ(de.Value().table_id, ct.Value().table_id);
    EXPECT_EQ(de.Value().col_names.size(), 3u);
    EXPECT_EQ(c->DescribeTable("nope").Error(), Err::kTableNotFound);

    ASSERT_TRUE(c->DropTable("t_api").IsOk());
    EXPECT_EQ(c->DescribeTable("t_api").Error(), Err::kTableNotFound);
    EXPECT_EQ(c->Scan("t_api").Error(), Err::kTableNotFound);
}

// ---------------- SCAN_PAGE 分页 ----------------

TEST_F(ClientTest, ScanPagedPages) {
    auto c = Connect();
    ASSERT_TRUE(c != nullptr);
    ASSERT_TRUE(c->CreateTable(MakeDef3("t_page")).IsOk());
    SeedViaEngine("t_page", 10);

    std::vector<size_t> sizes;
    std::vector<int64_t> seen;
    auto r = c->ScanPaged("t_page", 4, [&](const rtdb::ClientScanResult& page) {
        sizes.push_back(page.rows.size());
        for (const auto& row : page.rows) seen.push_back(row[0].as_int());
        return true;
    });
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), 10u);
    ASSERT_EQ(sizes.size(), 3u);  // 4+4+2
    EXPECT_EQ(sizes[0], 4u);
    EXPECT_EQ(sizes[1], 4u);
    EXPECT_EQ(sizes[2], 2u);
    ASSERT_EQ(seen.size(), 10u);
    for (size_t i = 0; i < seen.size(); ++i) {
        EXPECT_EQ(seen[i], static_cast<int64_t>(i + 1));  // 分页序 = 主键升序，无重无漏
    }

    // 提前停止：仅第一页
    sizes.clear();
    r = c->ScanPaged("t_page", 4, [&](const rtdb::ClientScanResult& page) {
        sizes.push_back(page.rows.size());
        return false;
    });
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), 4u);
    EXPECT_EQ(sizes.size(), 1u);

    // 非法页大小 → 101；未知表 → 303
    EXPECT_EQ(c->ScanPaged("t_page", 0, nullptr).Error(), Err::kInvalidArgument);
    EXPECT_EQ(c->ScanPaged("nope", 4, nullptr).Error(), Err::kTableNotFound);
}

// ---------------- 订阅：有序交付 ----------------

TEST_F(ClientTest, SubscribeDeliversInOrder) {
    auto c = Connect();
    ASSERT_TRUE(c != nullptr);
    ASSERT_TRUE(c->CreateTable(MakeDef3("t_sub")).IsOk());

    Collector col;
    auto sr = c->Subscribe(col.MakeOptions({"t_sub"}));
    ASSERT_TRUE(sr.IsOk());
    std::unique_ptr<rtdb::Subscription> sub = sr.TakeValue();
    ASSERT_TRUE(sub->WaitReady(5000)) << sub->last_error();
    EXPECT_TRUE(sub->connected());

    SeedViaEngine("t_sub", 5);
    ASSERT_TRUE(col.WaitCount(5, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto evs = col.Snapshot();
    ASSERT_EQ(evs.size(), 5u);
    uint64_t last = 0;
    for (size_t i = 0; i < evs.size(); ++i) {
        EXPECT_TRUE(evs[i].kind == rtdb::ChangeEvent::Kind::kInsert);
        EXPECT_EQ(evs[i].table, "t_sub");
        ASSERT_GE(evs[i].row.size(), 3u);
        EXPECT_EQ(evs[i].row[0].as_int(), static_cast<int64_t>(i + 1));
        EXPECT_EQ(evs[i].row[1].as_int(), static_cast<int64_t>(i + 1) * 10);
        EXPECT_EQ(evs[i].row[2].as_bytes(), "s" + std::to_string(i + 1));
        EXPECT_GT(evs[i].seq, last);
        last = evs[i].seq;
    }
    EXPECT_EQ(sub->position(), last);  // 续传点 = 最后交付 seq
    sub->Close();
    EXPECT_FALSE(sub->connected());
}

TEST_F(ClientTest, SubscribeRetriesUntilTableExists) {
    auto c = Connect();
    ASSERT_TRUE(c != nullptr);

    Collector col;
    auto sr = c->Subscribe(col.MakeOptions({"t_late"}));
    ASSERT_TRUE(sr.IsOk());
    std::unique_ptr<rtdb::Subscription> sub = sr.TakeValue();

    // 表未建：订阅重试中（不就绪、无事件）
    EXPECT_FALSE(sub->WaitReady(600));
    EXPECT_FALSE(sub->last_error().empty());

    ASSERT_TRUE(c->CreateTable(MakeDef3("t_late")).IsOk());
    ASSERT_TRUE(sub->WaitReady(8000)) << sub->last_error();

    SeedViaEngine("t_late", 1);
    ASSERT_TRUE(col.WaitCount(1, 5000));
    sub->Close();
}

// ---------------- 订阅：断线重连续传 ----------------

TEST_F(ClientTest, ReconnectResumesFromPosition) {
    auto c = Connect();
    ASSERT_TRUE(c != nullptr);
    ASSERT_TRUE(c->CreateTable(MakeDef3("t_rc")).IsOk());

    Collector col;
    auto sr = c->Subscribe(col.MakeOptions({"t_rc"}));
    ASSERT_TRUE(sr.IsOk());
    std::unique_ptr<rtdb::Subscription> sub = sr.TakeValue();
    ASSERT_TRUE(sub->WaitReady(5000)) << sub->last_error();

    SeedViaEngine("t_rc", 3);
    ASSERT_TRUE(col.WaitCount(3, 5000));
    const uint64_t p1 = sub->position();
    EXPECT_GT(p1, 0u);

    // 服务端重启（原端口、同映像；写入已 flush → committed 连续）
    const uint16_t keep_port = port;
    srv.Stop(2000);
    rtdb::server::Config cfg2 = cfg;
    cfg2.listen_port = keep_port;
    ASSERT_TRUE(rtdb::IsOk(srv.Start(cfg2))) << srv.last_error();
    port = srv.port();
    ASSERT_EQ(port, keep_port);

    // 泵自动重连 + 重发 AUTH/SUBSCRIBE（等待状态回调就绪）
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(10000);
        while (std::chrono::steady_clock::now() < deadline && !sub->connected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ASSERT_TRUE(sub->connected()) << sub->last_error();

    SeedViaEngine("t_rc", 3, 4);  // id 4..6
    ASSERT_TRUE(col.WaitCount(6, 8000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 无缺漏：id 1..6 都到；续传不重复投递已确认前缀（seq 严格递增）
    const auto evs = col.Snapshot();
    std::set<int64_t> ids;
    uint64_t last = 0;
    for (const rtdb::ChangeEvent& ev : evs) {
        EXPECT_GT(ev.seq, last);
        last = ev.seq;
        if (ev.kind == rtdb::ChangeEvent::Kind::kInsert && !ev.row.empty()) {
            ids.insert(ev.row[0].as_int());
        }
    }
    for (int i = 1; i <= 6; ++i) {
        EXPECT_EQ(ids.count(i), 1u) << "id=" << i;
    }
    EXPECT_GE(sub->position(), last);
    sub->Close();
}

// ---------------- 订阅：溢出重取（docs/04 §5 降级路径） ----------------

TEST_F(ClientTest, OverflowTriggersRescanAndReset) {
    auto c = Connect();
    ASSERT_TRUE(c != nullptr);

    rtdb::TableDef def = MakeDef2("t_ovf");
    def.row_cap = 2048;  // 三批共 1800 行（环 64KB 必然回收）
    ASSERT_TRUE(c->CreateTable(def).IsOk());

    Collector col;
    rtdb::SubscriptionOptions so = col.MakeOptions({"t_ovf"});
    so.delta_max_bytes = 64;  // 每批 1 条：消费远慢于写入，确保越窗
    so.on_overflow = [&](rtdb::Subscription& sub) {
        col.overflows.fetch_add(1);
        uint64_t committed = 0;
        auto p = c->Ping();
        if (p.IsOk() && p.Value().committed_seq >= 0) {
            committed = static_cast<uint64_t>(p.Value().committed_seq);
        }
        auto sc = c->ScanPaged("t_ovf", 256, [&](const rtdb::ClientScanResult& page) {
            col.rescanned.fetch_add(page.rows.size());
            return true;
        });
        if (!sc.IsOk()) {
            // 重扫失败不应发生；重置仍进行以免泵挂起
            ADD_FAILURE() << "溢出重扫失败：" << static_cast<int>(sc.Error());
        }
        sub.ResetTo(committed);
    };

    auto sr = c->Subscribe(so);
    ASSERT_TRUE(sr.IsOk());
    std::unique_ptr<rtdb::Subscription> sub = sr.TakeValue();
    ASSERT_TRUE(sub->WaitReady(5000)) << sub->last_error();

    WriteBatch600("t_ovf", 0);
    WriteBatch600("t_ovf", 600);
    WriteBatch600("t_ovf", 1200);

    // 突发写完成后的稳态判据：溢出已发生、且泵位置追平 committed
    // （溢出回调内联重扫 + ResetTo；多次溢出/续传拉平都在此收敛）。
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(20000);
        while (std::chrono::steady_clock::now() < deadline) {
            const uint64_t committed = static_cast<uint64_t>(srv.engine()->CommittedSeq());
            if (col.overflows.load() >= 1 && sub->position() >= committed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    EXPECT_GE(col.overflows.load(), 1) << "滑动窗口越窗未触发；err=" << sub->last_error();

    // 重同步收敛后补写一条：续传路径（事件交付）必达
    SeedRow2("t_ovf", 9000);
    ASSERT_TRUE(col.WaitRow(9000, 10000))
        << "overflows=" << col.overflows.load() << " rescanned=" << col.rescanned.load()
        << " count=" << col.Count() << " pos=" << sub->position()
        << " conn=" << sub->connected() << " err=" << sub->last_error()
        << " committed=" << srv.engine()->CommittedSeq();
    EXPECT_GE(col.rescanned.load(), 600u);  // 重扫覆盖被回收窗口
    EXPECT_TRUE(sub->connected());
    sub->Close();
}

// ---------------- 多订阅并发 ----------------

TEST_F(ClientTest, MultipleSubscriptionsConcurrent) {
    auto c = Connect();
    ASSERT_TRUE(c != nullptr);
    ASSERT_TRUE(c->CreateTable(MakeDef3("t_a")).IsOk());
    ASSERT_TRUE(c->CreateTable(MakeDef3("t_b")).IsOk());

    Collector ca;
    Collector cb;
    auto ra = c->Subscribe(ca.MakeOptions({"t_a"}));
    auto rb = c->Subscribe(cb.MakeOptions({"t_b"}));
    ASSERT_TRUE(ra.IsOk());
    ASSERT_TRUE(rb.IsOk());
    std::unique_ptr<rtdb::Subscription> sa = ra.TakeValue();
    std::unique_ptr<rtdb::Subscription> sb = rb.TakeValue();
    ASSERT_TRUE(sa->WaitReady(5000));
    ASSERT_TRUE(sb->WaitReady(5000));

    SeedViaEngine("t_a", 2);
    SeedViaEngine("t_b", 3);
    ASSERT_TRUE(ca.WaitCount(2, 5000));
    ASSERT_TRUE(cb.WaitCount(3, 5000));

    const auto ea = ca.Snapshot();
    const auto eb = cb.Snapshot();
    ASSERT_EQ(ea.size(), 2u);
    ASSERT_EQ(eb.size(), 3u);
    for (const auto& ev : ea) EXPECT_EQ(ev.table, "t_a");
    for (const auto& ev : eb) EXPECT_EQ(ev.table, "t_b");
    EXPECT_GT(sa->position(), 0u);
    EXPECT_GT(sb->position(), 0u);

    sa->Close();
    sb->Close();
    EXPECT_FALSE(sa->connected());
    EXPECT_FALSE(sb->connected());
}

// ---------------- C ABI 精简面 ----------------

TEST_F(ClientTest, CApiConnectScanSubscribe) {
    auto c = Connect();
    ASSERT_TRUE(c != nullptr);
    ASSERT_TRUE(c->CreateTable(MakeDef3("t_capi")).IsOk());
    SeedViaEngine("t_capi", 3);

    rtdb_client_options o{};
    o.host = "127.0.0.1";
    o.port = port;
    o.user = kUser;
    o.password = kPassword;
    o.auth_salt = kSalt;
    o.connect_timeout_ms = 2000;
    o.request_timeout_ms = 3000;
    rtdb_client* cc = nullptr;
    ASSERT_EQ(rtdb_client_connect(&o, &cc), RTDB_OK);
    ASSERT_TRUE(cc != nullptr);

    int64_t committed = -1;
    uint64_t hb = 0;
    uint32_t ttl = 0;
    ASSERT_EQ(rtdb_client_ping(cc, &committed, &hb, &ttl), RTDB_OK);
    EXPECT_GE(committed, 0);
    EXPECT_GT(hb, 0u);
    EXPECT_EQ(ttl, 30u * 60u * 1000u);

    // C 建表/删表
    rtdb_column_def cdefs[2]{};
    cdefs[0].name = "id";
    cdefs[0].type = RTDB_T_INT64;
    cdefs[0].nullable = 0;
    cdefs[1].name = "s";
    cdefs[1].type = RTDB_T_UTF8VAR;
    cdefs[1].nullable = 1;
    ASSERT_EQ(rtdb_client_create_table(cc, "t_capi_c", cdefs, 2, 128), RTDB_OK);
    ASSERT_EQ(rtdb_client_drop_table(cc, "t_capi_c"), RTDB_OK);

    // 扫描（含变长值回溯）
    rtdb_rows rows{};
    ASSERT_EQ(rtdb_client_scan(cc, "t_capi", nullptr, nullptr, &rows), RTDB_OK);
    ASSERT_EQ(rows.nrows, 3u);
    for (size_t i = 0; i < rows.nrows; ++i) {
        ASSERT_EQ(rows.rows[i].nvals, 3u);
        EXPECT_EQ(rows.rows[i].vals[0].int_value, static_cast<int64_t>(i + 1));
        EXPECT_EQ(rows.rows[i].vals[1].int_value, static_cast<int64_t>(i + 1) * 10);
        EXPECT_EQ(rows.rows[i].vals[2].type, RTDB_T_UTF8VAR);
        ASSERT_TRUE(rows.rows[i].vals[2].bytes != nullptr);
        EXPECT_EQ(std::string(rows.rows[i].vals[2].bytes, rows.rows[i].vals[2].bytes_len),
                  "s" + std::to_string(i + 1));
    }
    rtdb_client_free_rows(&rows);
    EXPECT_EQ(rows.nrows, 0u);
    rtdb_client_free_rows(&rows);      // 重复释放安全
    rtdb_client_free_rows(nullptr);    // NULL 安全

    // 块写：id 10（变长列经 lengths 承载）
    const int64_t cid[1] = {10};
    const int64_t cval[1] = {100};
    const char* const cmsg_ptr[1] = {"c10"};
    const uint32_t cmsg_len[1] = {3};
    rtdb_column cols[3]{};
    cols[0].name = "id";
    cols[0].type = RTDB_T_INT64;
    cols[0].data = cid;
    cols[1].name = "val";
    cols[1].type = RTDB_T_INT64;
    cols[1].data = cval;
    cols[2].name = "msg";
    cols[2].type = RTDB_T_UTF8VAR;
    cols[2].data = cmsg_ptr;
    cols[2].lengths = cmsg_len;
    uint64_t seq = 0;
    ASSERT_EQ(rtdb_client_upsert_block(cc, "t_capi", cols, 3, 1, &seq), RTDB_OK);
    EXPECT_GT(seq, 0u);

    ASSERT_EQ(rtdb_client_scan(cc, "t_capi", nullptr, nullptr, &rows), RTDB_OK);
    ASSERT_EQ(rows.nrows, 4u);
    EXPECT_EQ(rows.rows[3].vals[0].int_value, 10);
    EXPECT_EQ(std::string(rows.rows[3].vals[2].bytes, rows.rows[3].vals[2].bytes_len), "c10");
    rtdb_client_free_rows(&rows);

    // C 订阅：回调在泵线程触发（仅在回调内有效）
    struct CbState {
        std::atomic<int> count{0};
        std::atomic<int> overflows{0};
        std::atomic<int64_t> last_id{0};
        std::mutex mu;
        std::string last_table;
    };
    CbState st;
    rtdb_client_sub* sub = nullptr;
    ASSERT_EQ(rtdb_client_subscribe(
                  cc, nullptr, 0,
                  [](const rtdb_change* chg, void* user) {
                      auto* s = static_cast<CbState*>(user);
                      s->count.fetch_add(1);
                      if (chg->kind == RTDB_CHG_INSERT && chg->row.nvals > 0) {
                          s->last_id.store(chg->row.vals[0].int_value);
                      }
                      if (chg->table != nullptr) {
                          std::lock_guard<std::mutex> lk(s->mu);
                          s->last_table = chg->table;
                      }
                  },
                  [](void* user) { static_cast<CbState*>(user)->overflows.fetch_add(1); },
                  nullptr, &st, &sub),
              RTDB_OK);
    ASSERT_TRUE(sub != nullptr);

    // 订阅建立（全表）后写入两行；等待 C 回调收到
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline &&
               rtdb_client_sub_position(sub) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    SeedViaEngine("t_capi", 2, 11);  // id 11/12
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline && st.count.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    EXPECT_GE(st.count.load(), 2);
    EXPECT_EQ(st.overflows.load(), 0);
    EXPECT_EQ(st.last_id.load(), 12);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        EXPECT_EQ(st.last_table, "t_capi");
    }
    EXPECT_GT(rtdb_client_sub_position(sub), 0u);

    rtdb_client_sub_close(sub);
    rtdb_client_close(cc);
}

}  // namespace
