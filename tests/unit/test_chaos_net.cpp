/// M3-W5 网络混沌：真实 `rtdbd` 进程 kill -9 × N → 原端口重启 → 客户端
/// 请求面自动重连 + 订阅泵重连续传（docs/13 §5-W5②/§6）。
///
/// 两个场景：
///  1. `Kill9RestartResumesDelivery`：每轮写一批（等交付确认）→ 硬杀
///     （TerminateProcess / SIGKILL，不走优雅退出）→ 校验 PING 失效 →
///     同映像同端口重启 → 等订阅重连续传 → 写下一批；
///  2. `KillDuringWriteBurstSustainsDelivery`：先预置 64 键（全部应答并
///     交付确认），随后写线程对既有键持续 update（唯一 tag 标记，节流
///     2ms），主线程在写流中杀/重启 ×3——校验 **所有已应答（acked）写
///     入最终必达**（写路径不自动重发，则不得缺交付；允许重复）。
///
/// 语义依据（docs/13 §8-W5 记录）：影像脏页由 OS 持有，**进程终止不丢已
/// 提交数据**（M1 chaos 同结论：kill -9 后 committed_seq 一致、无半截记
/// 录），故重启后游标连续、交付允许重复但不允许缺漏；场景 1 按最强判据
/// 断言"无缺漏 + seq 严格递增 + 重启后心跳前进"，场景 2 按"acked ⊆ 交付"
/// 断言（记录数刻意远小于环容量，越窗视为场景失配即失败）。
///
/// 仅在编入 `RTDBD_EXE_PATH`（测试目标已提供，指向真实 rtdbd 产物）时启用。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "platform/net.hpp"
#include "rtdb/rtdb_client.hpp"
#include "rtdb/table_def.hpp"
#include "server/config.hpp"
#include "tests/unit/spawn_self.hpp"

namespace fs = std::filesystem;

#if defined(RTDBD_EXE_PATH)

namespace {

constexpr const char* kSalt = "chaos-salt";
constexpr const char* kUser = "chaos";
constexpr const char* kPassword = "chaos-pw";
constexpr const char* kTable = "t_chaos";

/// 事件汇（泵线程写、测试主线程读）：交付 seq 序列 + 前两列值集合 +
/// 状态迁移计数 + 溢出标记。列集合判据覆盖两个场景（场景 1 用列 0=id
/// 前缀无缺漏；场景 2 用列 1=tag 全含）。
struct EventSink {
    std::mutex mu;
    std::condition_variable cv;
    std::set<int64_t> col0;
    std::set<int64_t> col1;
    std::vector<uint64_t> seqs;
    int state_true = 0;
    int state_false = 0;
    std::atomic<bool> overflow{false};

    void InsertRow(const std::vector<rtdb::ColumnValue>& row) {
        if (row.size() >= 1 && !row[0].is_null()) col0.insert(row[0].as_int());
        if (row.size() >= 2 && !row[1].is_null()) col1.insert(row[1].as_int());
    }

    void OnChange(const rtdb::ChangeEvent& ev) {
        std::lock_guard<std::mutex> lk(mu);
        seqs.push_back(ev.seq);
        InsertRow(ev.row);
        cv.notify_all();
    }

    void OnState(bool ready) {
        std::lock_guard<std::mutex> lk(mu);
        (ready ? state_true : state_false)++;
        cv.notify_all();
    }

    /// 溢出重扫页（视为已见；判据见文件头）。
    void AddScannedPage(const rtdb::ClientScanResult& page) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& row : page.rows) InsertRow(row);
        cv.notify_all();
    }

    template <typename Pred>
    bool WaitPred(Pred pred, uint32_t budget_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(budget_ms), pred);
    }

    /// 等 id 1..upto 全部已见（前缀无缺漏判据）。
    bool WaitCol0Prefix(int64_t upto, uint32_t budget_ms) {
        return WaitPred(
            [&] {
                for (int64_t id = 1; id <= upto; ++id) {
                    if (col0.count(id) == 0) return false;
                }
                return true;
            },
            budget_ms);
    }
};

/// 单个混沌环境（映像路径 + 端口 + 认证摘要）。
struct ChaosEnv {
    std::string path;
    uint16_t port = 0;
    std::string digest;
};

ChaosEnv MakeEnv(const std::string& path) {
    ChaosEnv env;
    env.path = path;
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path + ".snap", ec);
    auto tmp = rtdb::platform::TcpListener::Listen("127.0.0.1", 0, 4);
    if (tmp.IsOk()) {
        env.port = tmp.Value().port();
        tmp.Value().Close();
    }
    env.digest = rtdb::server::DigestHex(rtdb::server::HashPassword(kSalt, kPassword));
    return env;
}

rtdb_test::ChildProcess StartRtdbd(const ChaosEnv& env) {
    std::vector<std::string> args;
    args.push_back("--path");
    args.push_back(env.path);
    args.push_back("--instance");
    args.push_back("chaos_net");
    args.push_back("--listen");
    args.push_back("127.0.0.1:" + std::to_string(env.port));
    args.push_back("--auth-salt");
    args.push_back(kSalt);
    args.push_back("--user");
    args.push_back(std::string(kUser) + "=" + env.digest);
    return rtdb_test::ChildProcess::Spawn(RTDBD_EXE_PATH, args);
}

rtdb::ClientOptions MakeOpts(const ChaosEnv& env) {
    rtdb::ClientOptions o;
    o.host = "127.0.0.1";
    o.port = env.port;
    o.user = kUser;
    o.password = kPassword;
    o.auth_salt = kSalt;
    o.connect_timeout_ms = 1000;
    o.request_timeout_ms = 3000;
    return o;
}

std::unique_ptr<rtdb::Client> ConnectReady(const ChaosEnv& env, uint32_t budget_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto r = rtdb::Client::Connect(MakeOpts(env));
        if (r.IsOk()) return r.TakeValue();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return nullptr;
}

/// 按场景断言"越窗即失配"的溢出处理器（整表重取 + ResetTo）。
std::function<void(rtdb::Subscription&)> MakeOverflowHandler(rtdb::Client& c, EventSink& sink) {
    return [&c, &sink](rtdb::Subscription& sub) {
        sink.overflow.store(true);
        uint64_t committed = 0;
        auto p = c.Ping();
        if (p.IsOk() && p.Value().committed_seq >= 0) {
            committed = static_cast<uint64_t>(p.Value().committed_seq);
        }
        auto s = c.ScanPaged(kTable, 128, [&sink](const rtdb::ClientScanResult& page) {
            sink.AddScannedPage(page);
            return true;
        });
        EXPECT_TRUE(s.IsOk());
        sub.ResetTo(committed);
    };
}

std::unique_ptr<rtdb::Subscription> SubscribeReady(rtdb::Client& c, EventSink& sink) {
    rtdb::SubscriptionOptions so;
    so.tables = {kTable};
    so.delta_wait_ms = 200;
    so.reconnect_min_ms = 100;
    so.reconnect_max_ms = 300;
    so.on_change = [&sink](const rtdb::ChangeEvent& ev) { sink.OnChange(ev); };
    so.on_state = [&sink](bool ready, uint32_t) { sink.OnState(ready); };
    so.on_overflow = MakeOverflowHandler(c, sink);
    auto sr = c.Subscribe(so);
    if (!sr.IsOk()) return nullptr;
    auto sub = sr.TakeValue();
    if (!sub->WaitReady(8000)) return nullptr;
    return sub;
}

bool WaitConnected(const std::unique_ptr<rtdb::Subscription>& sub, uint32_t budget_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline && !sub->connected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return sub->connected();
}

}  // namespace

TEST(NetChaos, Kill9RestartResumesDelivery) {
    const ChaosEnv env = MakeEnv("rtdb_gtest_chaos_net.rtdb");
    ASSERT_NE(env.port, 0);

    rtdb_test::ChildProcess srv = StartRtdbd(env);
    ASSERT_TRUE(srv.spawned());

    std::unique_ptr<rtdb::Client> c = ConnectReady(env, 10000);
    ASSERT_TRUE(c != nullptr) << "rtdbd 未在预算内就绪";

    rtdb::TableDef def;
    def.name = kTable;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
    def.row_cap = 512;
    auto ct = c->CreateTable(def);
    ASSERT_TRUE(ct.IsOk()) << rtdb::ErrorMessage(ct.Error());

    EventSink sink;
    std::unique_ptr<rtdb::Subscription> sub = SubscribeReady(*c, sink);
    ASSERT_TRUE(sub != nullptr) << "订阅未就绪";

    auto ping = c->Ping();
    ASSERT_TRUE(ping.IsOk());
    const uint64_t hb_before = ping.Value().writer_heartbeat_ns;

    int64_t next_id = 1;
    auto write_one = [&]() -> int64_t {
        if (next_id > 1000000) return -1;
        rtdb::ClientBatchOp op;
        op.type = rtdb::ClientBatchOp::Type::kInsert;
        op.table = kTable;
        op.row = {rtdb::ColumnValue::Int(next_id), rtdb::ColumnValue::Int(next_id * 10)};
        auto r = c->ExecuteBatch({op});
        if (!r.IsOk()) return -1;
        return next_id++;
    };

    constexpr int kRounds = 3;
    constexpr int kRowsPerRound = 8;
    for (int r = 0; r < kRounds; ++r) {
        // ---- 写一批 + 等交付（写路径在 kill 前已提交）----
        for (int i = 0; i < kRowsPerRound; ++i) {
            ASSERT_GT(write_one(), 0) << "第 " << r << " 轮第 " << i << " 条写入失败";
        }
        const int64_t top = next_id - 1;
        ASSERT_TRUE(sink.WaitCol0Prefix(top, 10000))
            << "第 " << r << " 轮交付缺漏（top=" << top << "，err=" << sub->last_error() << "）";

        // ---- 硬杀（不走优雅退出）----
        srv.Kill9();
        const int rc = srv.Wait();
        EXPECT_TRUE(rc == 9 || rc == 137) << "硬杀退出码异常：" << rc;

        // ---- PING 失效（写者活性探询：请求面自动重连失败）----
        const auto p_dead = c->Ping();
        EXPECT_FALSE(p_dead.IsOk()) << "服务端已杀但仍可 PING";

        // ---- 原端口/原映像重启 + 泵自动重连续传 ----
        srv = StartRtdbd(env);
        ASSERT_TRUE(srv.spawned());
        ASSERT_TRUE(WaitConnected(sub, 10000))
            << "第 " << r << " 轮重启后泵未重连：err=" << sub->last_error();
    }

    // ---- 终检：无缺漏 + 交付 seq 严格递增 + 心跳前进 ----
    const int64_t top = next_id - 1;
    std::set<int64_t> seen;
    std::vector<uint64_t> seqs;
    int st_true = 0, st_false = 0;
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        seen = sink.col0;
        seqs = sink.seqs;
        st_true = sink.state_true;
        st_false = sink.state_false;
    }
    for (int64_t id = 1; id <= top; ++id) {
        EXPECT_EQ(seen.count(id), 1u) << "行 id=" << id << " 未交付";
    }
    ASSERT_GE(seqs.size(), static_cast<size_t>(top));
    for (size_t i = 1; i < seqs.size(); ++i) {
        EXPECT_GT(seqs[i], seqs[i - 1]) << "seq 交付非严格递增（i=" << i << "）";
    }
    EXPECT_GE(st_true, kRounds + 1);  // 首连 + 每轮重启重连
    EXPECT_GE(st_false, kRounds);     // 每轮硬杀至少一次断连通知
    EXPECT_FALSE(sink.overflow.load());

    auto ping2 = c->Ping();
    ASSERT_TRUE(ping2.IsOk());
    EXPECT_GT(ping2.Value().writer_heartbeat_ns, hb_before) << "重启后写者心跳未前进";

    sub->Close();
    c->Close();
    srv.Kill9();
    (void)srv.Wait();
    std::error_code ec;
    fs::remove(env.path, ec);
    fs::remove(env.path + ".snap", ec);
}

TEST(NetChaos, KillDuringWriteBurstSustainsDelivery) {
    const ChaosEnv env = MakeEnv("rtdb_gtest_chaos_burst.rtdb");
    ASSERT_NE(env.port, 0);

    rtdb_test::ChildProcess srv = StartRtdbd(env);
    ASSERT_TRUE(srv.spawned());

    std::unique_ptr<rtdb::Client> c = ConnectReady(env, 10000);
    ASSERT_TRUE(c != nullptr) << "rtdbd 未在预算内就绪";

    rtdb::TableDef def;
    def.name = kTable;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"tag", rtdb::DataType::kInt64, 0, true, false});
    def.row_cap = 1024;
    auto ct = c->CreateTable(def);
    ASSERT_TRUE(ct.IsOk()) << rtdb::ErrorMessage(ct.Error());

    EventSink sink;
    std::unique_ptr<rtdb::Subscription> sub = SubscribeReady(*c, sink);
    ASSERT_TRUE(sub != nullptr) << "订阅未就绪";

    // ---- 阶段 A：预置 64 键（先建行，风暴全部走 update 不依赖键存在性）----
    constexpr int64_t kPool = 64;
    for (int64_t id = 1; id <= kPool; ++id) {
        rtdb::ClientBatchOp op;
        op.type = rtdb::ClientBatchOp::Type::kInsert;
        op.table = kTable;
        op.row = {rtdb::ColumnValue::Int(id), rtdb::ColumnValue::Int(id)};
        auto r = c->ExecuteBatch({op});
        ASSERT_TRUE(r.IsOk()) << "预置键 id=" << id << " 失败：" << rtdb::ErrorMessage(r.Error());
    }
    ASSERT_TRUE(sink.WaitCol0Prefix(kPool, 10000)) << "预置键交付缺漏";

    // ---- 阶段 B：写线程对既有键持续 update（tag 唯一标记交付比对）----
    // 节流 2ms（Windows 定时器粒度实际 ~15.6ms）；写类请求不自动重发，
    // 杀中断时该次调用失败计入 failed，后续调用自动重连继续。
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> attempts{0};
    std::mutex fmu;
    std::map<std::string, uint64_t> failed_msgs;  // 失败错误分类（诊断）
    std::vector<int64_t> acked;  // 仅写线程写、join 后主线程读
    const auto writer_t0 = std::chrono::steady_clock::now();
    std::thread writer([&] {
        int64_t tag = kPool;
        int64_t step = 0;
        while (!stop.load()) {
            ++tag;
            ++step;
            ++attempts;
            const int64_t key = (step - 1) % kPool + 1;
            rtdb::ClientBatchOp op;
            op.type = rtdb::ClientBatchOp::Type::kUpdate;
            op.table = kTable;
            op.row = {rtdb::ColumnValue::Int(key), rtdb::ColumnValue::Int(tag)};
            auto r = c->ExecuteBatch({op});
            if (r.IsOk()) {
                acked.push_back(tag);
            } else {
                failed.fetch_add(1);
                std::lock_guard<std::mutex> lk(fmu);
                failed_msgs[rtdb::ErrorMessage(r.Error())]++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // ---- 随机时机硬杀 ×3（写路径在飞行中被打断）----
    constexpr int kRounds = 3;
    for (int r = 0; r < kRounds; ++r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        srv.Kill9();
        const int rc = srv.Wait();
        EXPECT_TRUE(rc == 9 || rc == 137) << "硬杀退出码异常：" << rc;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        srv = StartRtdbd(env);
        ASSERT_TRUE(srv.spawned());
        ASSERT_TRUE(WaitConnected(sub, 10000))
            << "第 " << r << " 轮重启后泵未重连：err=" << sub->last_error();
    }
    // 尾窗：最后一轮重启后再放行一段写流，保证场景含"重启后正常写入"。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    stop.store(true);
    writer.join();
    std::printf("NetChaos burst 写线程：attempts=%llu acked=%zu failed=%llu elapsed_ms=%lld\n",
                static_cast<unsigned long long>(attempts.load()), acked.size(),
                static_cast<unsigned long long>(failed.load()),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - writer_t0)
                                           .count()));

    // ---- 收尾排空：等交付集合稳定（连续 500ms 无新事件且泵在线）----
    // 注意：`position()` 是"最后交付 seq"——批标记记录（BEGIN/COMMIT）不产
    // 生变更事件，末尾可能恰好停在 `committed-1`，不能与 committed 直接比齐；
    // 排空判据 = 交付集停增（泵延迟亚毫秒，500ms 稳定窗口足够）。
    bool drained = false;
    {
        size_t last_count = 0;
        int stable_ms = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lk(sink.mu);
                n = sink.col1.size();
            }
            if (n == last_count) {
                stable_ms += 50;
            } else {
                last_count = n;
                stable_ms = 0;
            }
            if (stable_ms >= 500 && sub->connected()) {
                drained = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (!drained) {
        auto p = c->Ping();
        int st_true = 0, st_false = 0;
        {
            std::lock_guard<std::mutex> lk(sink.mu);
            st_true = sink.state_true;
            st_false = sink.state_false;
        }
        std::fprintf(stderr,
                     "[chaos] 排空未完成：connected=%d position=%llu committed=%lld ping_ok=%d "
                     "overflow=%d state(+%d/-%d) err=%s\n",
                     sub->connected() ? 1 : 0,
                     static_cast<unsigned long long>(sub->position()),
                     p.IsOk() ? static_cast<long long>(p.Value().committed_seq) : -1,
                     p.IsOk() ? 1 : 0, sink.overflow.load() ? 1 : 0, st_true, st_false,
                     sub->last_error().c_str());
    }
    ASSERT_TRUE(drained) << "重启后未在预算内追平（err=" << sub->last_error() << "）";

    // ---- 终检：所有已应答写入必达（acked ⊆ 交付 tag 集）----
    std::set<int64_t> seen_tags;
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        seen_tags = sink.col1;
    }
    {
        std::lock_guard<std::mutex> lk(fmu);
        for (const auto& kv : failed_msgs) {
            std::printf("NetChaos burst failed[%s]=%llu\n", kv.first.c_str(),
                        static_cast<unsigned long long>(kv.second));
        }
    }
    ASSERT_GT(acked.size(), 50u) << "写负载过小，场景无效（acked=" << acked.size()
                                 << " failed=" << failed.load() << " attempts="
                                 << attempts.load() << "）";
    size_t missing = 0;
    for (const int64_t tag : acked) {
        if (seen_tags.count(tag) == 0) {
            if (missing < 5) {
                ADD_FAILURE() << "acked tag=" << tag << " 未交付";
            }
            ++missing;
        }
    }
    EXPECT_EQ(missing, 0u) << "acked 写入缺交付共 " << missing << " 条";
    EXPECT_FALSE(sink.overflow.load());
    std::printf("NetChaos burst：acked=%zu failed=%llu 交付 unique_tags=%zu\n", acked.size(),
                static_cast<unsigned long long>(failed.load()), seen_tags.size());

    sub->Close();
    c->Close();
    srv.Kill9();
    (void)srv.Wait();
    std::error_code ec;
    fs::remove(env.path, ec);
    fs::remove(env.path + ".snap", ec);
}

#endif  // RTDBD_EXE_PATH
