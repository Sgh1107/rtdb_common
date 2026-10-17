/// M3-W3 服务端（rtdbd）集成测试：协议全功能面（AUTH/PING/DDL/EXEC_BATCH/
/// SCAN/SCAN_PAGE/订阅族）、错误路由、会话上限与 TTL、优雅退出，另含
/// rtdbd 可执行拉起烟测（docs/13 §5-W3）。
///
/// 本文件自带最小协议客户端（阻塞 socket + net/wire 复用）；正式客户端
/// 归 W4（rtdb_client），两处刻意独立以便交叉验证协议口径。

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "net/codec.hpp"
#include "net/value_codec.hpp"
#include "net/wire.hpp"
#include "platform/net.hpp"
#include "rtdb/engine.hpp"
#include "rtdb/proto_v1.h"
#include "rtdb/record_block.hpp"
#include "server/config.hpp"
#include "server/server.hpp"
#include "tests/unit/spawn_self.hpp"

namespace fs = std::filesystem;

namespace {

using rtdb::Err;

// ---------------- 最小协议客户端 ----------------

struct Client {
    rtdb::platform::TcpSocket sock;
    uint32_t session = 0;

    bool Connect(uint16_t port) {
        auto c = rtdb::platform::TcpSocket::Connect("127.0.0.1", port, 3000);
        if (!c.IsOk()) return false;
        sock = c.TakeValue();
        sock.SetTimeout(5000);
        return true;
    }

    void Close() { sock.Close(); }

    /// 发送请求帧并读应答：返回业务错误码；连接层/帧层异常 → kNetworkError。
    Err Call(uint16_t func, const std::vector<uint8_t>& body, std::vector<uint8_t>* out) {
        rtdb::net::FrameHeader h;
        h.ver = rtdb::net::kProtoVersion;
        h.flags = 0;
        h.session = session;
        h.func = func;
        h.payload_len = static_cast<uint32_t>(body.size());
        if (!rtdb::IsOk(rtdb::net::WriteFrame(sock, h, body.empty() ? nullptr : body.data()))) {
            return Err::kNetworkError;
        }
        rtdb::net::FrameHeader rh;
        std::vector<uint8_t> resp;
        if (!rtdb::IsOk(
                rtdb::net::ReadFrame(sock, rtdb::net::kDefaultMaxPayloadBytes, &rh, &resp))) {
            return Err::kNetworkError;
        }
        if (rh.func != func || rh.flags != rtdb::net::kFlagResp) return Err::kNetworkError;
        if (rh.session != h.session) return Err::kNetworkError;  // 会话号回显口径
        if (resp.size() < 2) return Err::kNetworkError;

        const int16_t code = static_cast<int16_t>(
            static_cast<uint16_t>(resp[0] | (static_cast<uint16_t>(resp[1]) << 8)));
        const Err re = static_cast<Err>(code);
        size_t off = 2;
        if (re != Err::kOk) {
            if (resp.size() < 4) return Err::kNetworkError;
            const uint16_t ml = static_cast<uint16_t>(resp[2] | (resp[3] << 8));
            off = 4 + static_cast<size_t>(ml);
            if (resp.size() < off) return Err::kNetworkError;
        }
        if (out != nullptr) {
            out->assign(resp.begin() + static_cast<std::ptrdiff_t>(off), resp.end());
        }
        return re;
    }

    Err Auth(const std::string& salt, const std::string& user, const std::string& password,
             uint32_t* ttl_ms = nullptr) {
        std::vector<uint8_t> b;
        rtdb::net::Writer w(&b);
        w.Str(user);
        const auto d = rtdb::server::HashPassword(salt, password);
        w.Var(std::string(reinterpret_cast<const char*>(d.data()), d.size()));
        std::vector<uint8_t> out;
        const Err e = Call(RTDB_PF_AUTH, b, &out);
        if (rtdb::IsOk(e)) {
            rtdb::net::Reader r(out.data(), out.size());
            session = r.U32();
            const uint32_t ttl = r.U32();
            if (ttl_ms != nullptr) *ttl_ms = ttl;
        }
        return e;
    }
};

// ---------------- 请求体构造 ----------------

rtdb::TableDef MakeDef2(const std::string& name) {
    rtdb::TableDef def;
    def.name = name;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
    return def;
}

rtdb::TableDef MakeDef3(const std::string& name) {
    rtdb::TableDef def;
    def.name = name;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
    def.columns.push_back({"s", rtdb::DataType::kUtf8Var, 0, true, false});
    return def;
}

std::vector<uint8_t> DdlBody(uint8_t sub, const rtdb::TableDef& def) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U8(sub);
    rtdb::net::WriteTableDef(w, def);
    return b;
}

std::vector<uint8_t> DdlNameBody(uint8_t sub, const std::string& name) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U8(sub);
    w.Str(name);
    return b;
}

struct OpSpec {
    uint8_t type = RTDB_BATCH_MODE_OPS;
    std::string table;
    std::vector<rtdb::ColumnValue> row;
};

std::vector<uint8_t> OpsBody(const std::vector<OpSpec>& ops) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U8(RTDB_BATCH_MODE_OPS);
    w.U32(static_cast<uint32_t>(ops.size()));
    for (const OpSpec& op : ops) {
        w.U8(op.type);
        w.Str(op.table);
        for (const rtdb::ColumnValue& v : op.row) rtdb::net::WriteValue(w, v);
    }
    return b;
}

std::vector<uint8_t> ScanBody(const std::string& table, bool has_lo, int64_t lo, bool has_hi,
                              int64_t hi) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.Str(table);
    w.U8(has_lo ? 1 : 0);
    if (has_lo) {
        w.U8(RTDB_VAL_I64);
        w.I64(lo);
    }
    w.U8(has_hi ? 1 : 0);
    if (has_hi) {
        w.U8(RTDB_VAL_I64);
        w.I64(hi);
    }
    return b;
}

std::vector<uint8_t> PageOpenBody(const std::string& table, uint32_t page_rows) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U8(RTDB_SP_OPEN);
    w.Str(table);
    w.U32(page_rows);
    return b;
}

std::vector<uint8_t> PageIdBody(uint8_t sub, uint64_t page_id) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U8(sub);
    w.U64(page_id);
    return b;
}

Err CreateViaDdl(Client& c, const rtdb::TableDef& def, int64_t* table_id = nullptr,
                 uint32_t* ncols = nullptr, uint32_t* layout_version = nullptr) {
    std::vector<uint8_t> out;
    const Err e = c.Call(RTDB_PF_DDL, DdlBody(RTDB_DDL_CREATE_TABLE, def), &out);
    if (rtdb::IsOk(e) && (table_id != nullptr || ncols != nullptr || layout_version != nullptr)) {
        rtdb::net::Reader r(out.data(), out.size());
        const int64_t tid = r.I64();
        const uint32_t nc = r.U32();
        const uint32_t lv = r.U32();
        if (table_id != nullptr) *table_id = tid;
        if (ncols != nullptr) *ncols = nc;
        if (layout_version != nullptr) *layout_version = lv;
    }
    return e;
}

// ---------------- 应答解析 ----------------

struct ScanOut {
    rtdb::net::ResultSchema schema;
    std::vector<std::vector<rtdb::ColumnValue>> rows;
};

bool ParseRows(const std::vector<uint8_t>& out, ScanOut* res) {
    rtdb::net::Reader r(out.data(), out.size());
    auto s = rtdb::net::ReadResultHeader(r);
    if (!s.IsOk()) return false;
    res->schema = s.TakeValue();
    res->rows.clear();
    for (uint32_t i = 0; i < res->schema.n_rows; ++i) {
        res->rows.push_back(rtdb::net::ReadRow(r, res->schema.col_types));
        if (!r.ok()) return false;
    }
    return r.ok();
}

struct ParsedChange {
    uint8_t kind = 0;
    int64_t table_id = 0;
    std::string table;
    uint64_t seq = 0;
    uint64_t ts_ns = 0;
    uint64_t row_no = 0;
    uint64_t row_ver = 0;
    std::vector<rtdb::ColumnValue> row;
    rtdb::ColumnValue key = rtdb::ColumnValue::Null();
};

bool ParseFetchOut(const std::vector<uint8_t>& out, int64_t* last_seq,
                   std::vector<ParsedChange>* evs) {
    rtdb::net::Reader r(out.data(), out.size());
    *last_seq = r.I64();
    const uint32_t n = r.U32();
    if (!r.ok()) return false;
    evs->clear();
    for (uint32_t i = 0; i < n; ++i) {
        ParsedChange e;
        e.kind = r.U8();
        e.table_id = r.I64();
        if (!r.Str(&e.table)) return false;
        e.seq = r.U64();
        e.ts_ns = r.U64();
        e.row_no = r.U64();
        e.row_ver = r.U64();
        if (e.kind == RTDB_CHG_DELETE) {
            e.key = rtdb::net::ReadValueGeneric(r);
        } else {
            const uint32_t nv = r.U32();
            for (uint32_t j = 0; j < nv && r.ok(); ++j) {
                e.row.push_back(rtdb::net::ReadValueGeneric(r));
            }
        }
        if (!r.ok()) return false;
        evs->push_back(std::move(e));
    }
    return r.Finish();
}

Err Subscribe(Client& c, const std::vector<std::string>& tables, uint32_t* sub_id,
              int64_t* start_seq) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U32(static_cast<uint32_t>(tables.size()));
    for (const std::string& t : tables) w.Str(t);
    std::vector<uint8_t> out;
    const Err e = c.Call(RTDB_PF_SUBSCRIBE, b, &out);
    if (rtdb::IsOk(e)) {
        rtdb::net::Reader r(out.data(), out.size());
        *sub_id = r.U32();
        *start_seq = r.I64();
    }
    return e;
}

Err FetchDelta(Client& c, uint32_t sub_id, uint64_t from, uint32_t timeout_ms,
               uint32_t max_bytes, std::vector<uint8_t>* out) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U32(sub_id);
    w.U64(from);
    w.U32(timeout_ms);
    w.U32(max_bytes);
    return c.Call(RTDB_PF_FETCH_DELTA, b, out);
}

Err Ack(Client& c, uint32_t sub_id, uint64_t seq) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U32(sub_id);
    w.U64(seq);
    return c.Call(RTDB_PF_ACK, b, nullptr);
}

Err Unregister(Client& c, uint32_t sub_id) {
    std::vector<uint8_t> b;
    rtdb::net::Writer w(&b);
    w.U32(sub_id);
    return c.Call(RTDB_PF_UNREGISTER, b, nullptr);
}

}  // namespace

// ---------------- 测试夹具 ----------------

class ServerTest : public ::testing::Test {
protected:
    static constexpr const char* kSalt = "unit-salt";
    static constexpr const char* kUser = "admin";
    static constexpr const char* kPassword = "pw-123";

    rtdb::server::Server srv;
    std::string path;
    uint16_t port = 0;

    /// 子类可改写配置（上限/TTL 等）。
    virtual void Configure(rtdb::server::Config& cfg) { (void)cfg; }

    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag = info->name();
        path = "rtdb_gtest_server_" + tag + ".rtdb";
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path + ".snap", ec);

        rtdb::server::Config cfg;
        cfg.instance = "srv_" + tag;
        cfg.path = path;
        cfg.initial_size_mb = 32;
        cfg.listen_host = "127.0.0.1";
        cfg.listen_port = 0;  // 系统分配；port() 取回
        cfg.auth_salt = kSalt;
        cfg.fsync = rtdb::FsyncPolicy::Never;
        rtdb::server::UserEntry u;
        u.name = kUser;
        u.digest = rtdb::server::HashPassword(cfg.auth_salt, kPassword);
        cfg.users.push_back(u);
        Configure(cfg);

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

    Client ConnectAuthed() {
        Client c;
        EXPECT_TRUE(c.Connect(port));
        EXPECT_EQ(c.Auth(kSalt, kUser, kPassword), Err::kOk);
        return c;
    }

    /// 收录 N 行（id = 1..n，val = id*10）并返回末行 seq 无关；走引擎直写。
    void SeedRows(const std::string& table, int n) {
        auto t = srv.engine()->OpenTable(table);
        ASSERT_TRUE(t.IsOk());
        for (int i = 1; i <= n; ++i) {
            auto r = srv.engine()->Insert(
                t.Value(), {rtdb::ColumnValue::Int(i), rtdb::ColumnValue::Int(i * 10)});
            ASSERT_TRUE(r.IsOk());
        }
    }
};

// ---------------- 认证/PING ----------------

TEST_F(ServerTest, AuthAndPing) {
    Client c;
    ASSERT_TRUE(c.Connect(port));

    // 未认证：会话门 502
    EXPECT_EQ(c.Call(RTDB_PF_PING, {}, nullptr), Err::kTokenExpired);
    // 口令错：501（连接保留可重试）
    EXPECT_EQ(c.Auth(kSalt, kUser, "wrong"), Err::kAuthFailed);
    // 正确口令
    uint32_t ttl = 0;
    ASSERT_EQ(c.Auth(kSalt, kUser, kPassword, &ttl), Err::kOk);
    EXPECT_NE(c.session, 0u);
    EXPECT_EQ(ttl, 30u * 60u * 1000u);

    std::vector<uint8_t> out;
    ASSERT_EQ(c.Call(RTDB_PF_PING, {}, &out), Err::kOk);
    rtdb::net::Reader r(out.data(), out.size());
    const int64_t committed = r.I64();
    const uint64_t hb = r.U64();
    (void)r.I64();  // checkpoint
    (void)r.U32();  // ttl
    EXPECT_TRUE(r.Finish());
    EXPECT_GE(committed, 0);
    EXPECT_GT(hb, 0u);  // Start 即刷新过一次心跳

    // 伪造会话号：502
    const uint32_t good = c.session;
    c.session = good + 1;
    EXPECT_EQ(c.Call(RTDB_PF_PING, {}, nullptr), Err::kTokenExpired);
    c.session = good;
    EXPECT_EQ(c.Call(RTDB_PF_PING, {}, nullptr), Err::kOk);

    // 空用户表之外的未知用户名：501
    Client c2;
    ASSERT_TRUE(c2.Connect(port));
    EXPECT_EQ(c2.Auth(kSalt, "nobody", kPassword), Err::kAuthFailed);

    EXPECT_GE(srv.stats().auth_ok, 1u);
    EXPECT_GE(srv.stats().auth_fail, 2u);  // wrong + nobody
}

TEST_F(ServerTest, UnknownFuncAndMalformedPayloadKeepConnection) {
    Client c = ConnectAuthed();
    EXPECT_EQ(c.Call(999, {}, nullptr), Err::kNotImplemented);      // 未知功能号 → 400
    EXPECT_EQ(c.Call(RTDB_PF_PING, {1, 2, 3}, nullptr), Err::kProtocolError);  // 畸形载荷 → 403
    // 两者都不应断开连接
    std::vector<uint8_t> out;
    EXPECT_EQ(c.Call(RTDB_PF_PING, {}, &out), Err::kOk);
    EXPECT_GE(srv.stats().payload_errors, 2u);
}

// ---------------- DDL ----------------

TEST_F(ServerTest, DdlCreateDescribeDrop) {
    Client c = ConnectAuthed();
    const rtdb::TableDef def = MakeDef3("t_ddl");

    int64_t tid = 0;
    uint32_t ncols = 0;
    uint32_t lv = 0;
    ASSERT_EQ(CreateViaDdl(c, def, &tid, &ncols, &lv), Err::kOk);
    EXPECT_GT(tid, 0);
    EXPECT_EQ(ncols, 3u);
    EXPECT_GE(lv, 1u);

    // 重名 → 302
    EXPECT_EQ(CreateViaDdl(c, def), Err::kTableExists);
    // 未知表描述 → 303
    std::vector<uint8_t> out;
    EXPECT_EQ(c.Call(RTDB_PF_DDL, DdlNameBody(RTDB_DDL_DESCRIBE_TABLE, "nope"), &out),
              Err::kTableNotFound);

    // DESCRIBE 自描述 schema 与建表一致
    ASSERT_EQ(c.Call(RTDB_PF_DDL, DdlNameBody(RTDB_DDL_DESCRIBE_TABLE, "t_ddl"), &out),
              Err::kOk);
    rtdb::net::Reader r(out.data(), out.size());
    EXPECT_EQ(r.I64(), tid);
    (void)r.U32();
    auto schema = rtdb::net::ReadResultHeader(r);
    ASSERT_TRUE(schema.IsOk());
    EXPECT_EQ(schema.Value().table, "t_ddl");
    EXPECT_EQ(schema.Value().col_names.size(), 3u);
    EXPECT_EQ(schema.Value().col_names[0], "id");
    EXPECT_EQ(schema.Value().col_types[2], rtdb::DataType::kUtf8Var);
    EXPECT_EQ(schema.Value().n_rows, 0u);

    // DROP → 重删 303 → 再描述 303
    EXPECT_EQ(c.Call(RTDB_PF_DDL, DdlNameBody(RTDB_DDL_DROP_TABLE, "t_ddl"), nullptr), Err::kOk);
    EXPECT_EQ(c.Call(RTDB_PF_DDL, DdlNameBody(RTDB_DDL_DROP_TABLE, "t_ddl"), nullptr),
              Err::kTableNotFound);
    EXPECT_EQ(c.Call(RTDB_PF_DDL, DdlNameBody(RTDB_DDL_DESCRIBE_TABLE, "t_ddl"), nullptr),
              Err::kTableNotFound);
}

// ---------------- EXEC_BATCH ----------------

TEST_F(ServerTest, ExecBatchOpsMode) {
    Client c = ConnectAuthed();
    ASSERT_EQ(CreateViaDdl(c, MakeDef3("t_ops")), Err::kOk);

    // 批 1：两行插入
    std::vector<uint8_t> out;
    std::vector<OpSpec> batch1;
    batch1.push_back({1, "t_ops",
                      {rtdb::ColumnValue::Int(10), rtdb::ColumnValue::Int(100),
                       rtdb::ColumnValue::Text("a")}});
    batch1.push_back({1, "t_ops",
                      {rtdb::ColumnValue::Int(20), rtdb::ColumnValue::Int(200),
                       rtdb::ColumnValue::Null()}});
    ASSERT_EQ(c.Call(RTDB_PF_EXEC_BATCH, OpsBody(batch1), &out), Err::kOk);
    rtdb::net::Reader r1(out.data(), out.size());
    EXPECT_GT(r1.I64(), 0);

    // 批 2：更新 10、删除 20（批内互不依赖：基于批前状态）
    std::vector<OpSpec> batch2;
    batch2.push_back({2, "t_ops",
                      {rtdb::ColumnValue::Int(10), rtdb::ColumnValue::Int(111),
                       rtdb::ColumnValue::Text("aa")}});
    batch2.push_back({3, "t_ops", {rtdb::ColumnValue::Int(20)}});
    ASSERT_EQ(c.Call(RTDB_PF_EXEC_BATCH, OpsBody(batch2), &out), Err::kOk);

    // SCAN 验证：仅剩 id=10，val=111，s="aa"
    ASSERT_EQ(c.Call(RTDB_PF_SCAN, ScanBody("t_ops", false, 0, false, 0), &out), Err::kOk);
    ScanOut res;
    ASSERT_TRUE(ParseRows(out, &res));
    ASSERT_EQ(res.rows.size(), 1u);
    EXPECT_EQ(res.rows[0][0].as_int(), 10);
    EXPECT_EQ(res.rows[0][1].as_int(), 111);
    EXPECT_FALSE(res.rows[0][2].is_null());
    EXPECT_EQ(res.rows[0][2].as_bytes(), "aa");

    // 校验失败整批拒绝：删除不存在的 99 → 304/303 类错误且无副作用
    std::vector<OpSpec> bad;
    bad.push_back({3, "t_ops", {rtdb::ColumnValue::Int(99)}});
    const Err bad_err = c.Call(RTDB_PF_EXEC_BATCH, OpsBody(bad), nullptr);
    EXPECT_FALSE(rtdb::IsOk(bad_err));

    // 未知表 → 303
    std::vector<OpSpec> missing;
    missing.push_back({1, "no_table", {rtdb::ColumnValue::Int(1)}});
    EXPECT_EQ(c.Call(RTDB_PF_EXEC_BATCH, OpsBody(missing), nullptr), Err::kTableNotFound);
}

TEST_F(ServerTest, ExecBatchBlockMode) {
    Client c = ConnectAuthed();
    ASSERT_EQ(CreateViaDdl(c, MakeDef2("t_blk")), Err::kOk);

    const std::vector<int64_t> ids{1, 2, 3};
    const std::vector<int64_t> vals{10, 20, 30};
    rtdb::RecordBlock blk("t_blk", 3);
    blk.AppendColumn("id", ids).AppendColumn("val", vals);
    ASSERT_TRUE(blk.IsValid());

    std::vector<uint8_t> body;
    rtdb::net::Writer w(&body);
    w.U8(RTDB_BATCH_MODE_BLOCK);
    rtdb::net::WriteBlock(w, blk.View());

    std::vector<uint8_t> out;
    ASSERT_EQ(c.Call(RTDB_PF_EXEC_BATCH, body, &out), Err::kOk);
    rtdb::net::Reader r0(out.data(), out.size());
    EXPECT_GT(r0.I64(), 0);

    // 同块再发（upsert 语义）：id 相同、值更新
    const std::vector<int64_t> vals2{11, 21, 31};
    rtdb::RecordBlock blk2("t_blk", 3);
    blk2.AppendColumn("id", ids).AppendColumn("val", vals2);
    std::vector<uint8_t> body2;
    rtdb::net::Writer w2(&body2);
    w2.U8(RTDB_BATCH_MODE_BLOCK);
    rtdb::net::WriteBlock(w2, blk2.View());
    ASSERT_EQ(c.Call(RTDB_PF_EXEC_BATCH, body2, nullptr), Err::kOk);

    ASSERT_EQ(c.Call(RTDB_PF_SCAN, ScanBody("t_blk", false, 0, false, 0), &out), Err::kOk);
    ScanOut res;
    ASSERT_TRUE(ParseRows(out, &res));
    ASSERT_EQ(res.rows.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(res.rows[i][0].as_int(), static_cast<int64_t>(i + 1));
        EXPECT_EQ(res.rows[i][1].as_int(), vals2[i]);
    }
}

// ---------------- SCAN / SCAN_PAGE ----------------

TEST_F(ServerTest, ScanRangeAndPaged) {
    Client c = ConnectAuthed();
    ASSERT_EQ(CreateViaDdl(c, MakeDef2("t_scan")), Err::kOk);
    SeedRows("t_scan", 25);

    // 左闭右闭区间 5..9
    std::vector<uint8_t> out;
    ASSERT_EQ(c.Call(RTDB_PF_SCAN, ScanBody("t_scan", true, 5, true, 9), &out), Err::kOk);
    ScanOut res;
    ASSERT_TRUE(ParseRows(out, &res));
    ASSERT_EQ(res.rows.size(), 5u);
    EXPECT_EQ(res.rows.front()[0].as_int(), 5);
    EXPECT_EQ(res.rows.back()[0].as_int(), 9);

    // 仅下界：5..25
    ASSERT_EQ(c.Call(RTDB_PF_SCAN, ScanBody("t_scan", true, 5, false, 0), &out), Err::kOk);
    ASSERT_TRUE(ParseRows(out, &res));
    EXPECT_EQ(res.rows.size(), 21u);
    EXPECT_EQ(res.rows.front()[0].as_int(), 5);
    EXPECT_EQ(res.rows.back()[0].as_int(), 25);

    // 分页：10+10+5，随后空页（done=1）
    ASSERT_EQ(c.Call(RTDB_PF_SCAN_PAGE, PageOpenBody("t_scan", 10), &out), Err::kOk);
    {
        rtdb::net::Reader r(out.data(), out.size());
        const uint64_t page_id = r.U64();
        const uint8_t done = r.U8();
        auto schema = rtdb::net::ReadResultHeader(r);
        ASSERT_TRUE(schema.IsOk());
        EXPECT_EQ(done, 0u);
        auto row = rtdb::net::ReadRow(r, schema.Value().col_types);
        EXPECT_EQ(row[0].as_int(), 1);

        int64_t last = 0;
        for (int page = 0; page < 2; ++page) {
            ASSERT_EQ(c.Call(RTDB_PF_SCAN_PAGE, PageIdBody(RTDB_SP_FETCH, page_id), &out),
                      Err::kOk);
            rtdb::net::Reader rf(out.data(), out.size());
            const uint8_t fdone = rf.U8();
            const uint32_t nrows = rf.U32();
            if (page == 0) {
                EXPECT_EQ(nrows, 10u);
                EXPECT_EQ(fdone, 0u);
            } else {
                EXPECT_EQ(nrows, 5u);
                EXPECT_EQ(fdone, 1u);
            }
            last = 0;
            for (uint32_t i = 0; i < nrows; ++i) {
                auto rowf = rtdb::net::ReadRow(rf, schema.Value().col_types);
                ASSERT_FALSE(rowf.empty());
                last = rowf[0].as_int();
            }
            EXPECT_TRUE(rf.Finish());
        }
        EXPECT_EQ(last, 25);

        // 已完页再取：0 行 + done=1
        ASSERT_EQ(c.Call(RTDB_PF_SCAN_PAGE, PageIdBody(RTDB_SP_FETCH, page_id), &out),
                  Err::kOk);
        {
            rtdb::net::Reader rf(out.data(), out.size());
            EXPECT_EQ(rf.U8(), 1u);
            EXPECT_EQ(rf.U32(), 0u);
            EXPECT_TRUE(rf.Finish());
        }

        // 关闭后再取 → 102
        ASSERT_EQ(c.Call(RTDB_PF_SCAN_PAGE, PageIdBody(RTDB_SP_CLOSE, page_id), nullptr),
                  Err::kOk);
        EXPECT_EQ(c.Call(RTDB_PF_SCAN_PAGE, PageIdBody(RTDB_SP_FETCH, page_id), nullptr),
                  Err::kNotFound);
    }

    // 非法页大小 → 101
    EXPECT_EQ(c.Call(RTDB_PF_SCAN_PAGE, PageOpenBody("t_scan", 0), nullptr),
              Err::kInvalidArgument);
    // 未知表 → 303
    EXPECT_EQ(c.Call(RTDB_PF_SCAN_PAGE, PageOpenBody("nope", 10), nullptr),
              Err::kTableNotFound);
}

// ---------------- 订阅族 ----------------

TEST_F(ServerTest, SubscribeFetchDeltaLifecycle) {
    Client c = ConnectAuthed();
    ASSERT_EQ(CreateViaDdl(c, MakeDef3("t_sub")), Err::kOk);

    auto t = srv.engine()->OpenTable("t_sub");
    ASSERT_TRUE(t.IsOk());

    uint32_t sub_id = 0;
    int64_t start_seq = 0;
    ASSERT_EQ(Subscribe(c, {"t_sub"}, &sub_id, &start_seq), Err::kOk);
    EXPECT_NE(sub_id, 0u);

    // 直写引擎：insert → update（先拉一次，验证"当前版本可读变长值"）
    ASSERT_TRUE(srv.engine()
                    ->Insert(t.Value(), {rtdb::ColumnValue::Int(1), rtdb::ColumnValue::Int(11),
                                         rtdb::ColumnValue::Text("aa")})
                    .IsOk());
    ASSERT_TRUE(srv.engine()
                    ->UpdateByKey(t.Value(), rtdb::ColumnValue::Int(1),
                                  {rtdb::ColumnValue::Int(1), rtdb::ColumnValue::Int(12),
                                   rtdb::ColumnValue::Text("bb")})
                    .IsOk());

    std::vector<uint8_t> out;
    ASSERT_EQ(FetchDelta(c, sub_id, static_cast<uint64_t>(start_seq), 2000, 1u << 20, &out),
              Err::kOk);
    int64_t last_seq = 0;
    std::vector<ParsedChange> evs;
    ASSERT_TRUE(ParseFetchOut(out, &last_seq, &evs));
    ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].kind, RTDB_CHG_INSERT);
    EXPECT_EQ(evs[1].kind, RTDB_CHG_UPDATE);
    EXPECT_EQ(evs[0].table, "t_sub");
    EXPECT_EQ(evs[0].table_id, t.Value().table_id());
    ASSERT_EQ(evs[0].row.size(), 3u);
    EXPECT_EQ(evs[0].row[0].as_int(), 1);
    EXPECT_EQ(evs[0].row[1].as_int(), 11);
    // 变长列版本门（docs/13 §8-W2）：insert 版本已非当前 → 该列 NULL。
    EXPECT_TRUE(evs[0].row[2].is_null());
    EXPECT_EQ(evs[1].row[1].as_int(), 12);
    EXPECT_EQ(evs[1].row[2].as_bytes(), "bb");  // update 为当前版本 → 真实值
    EXPECT_LT(evs[0].seq, evs[1].seq);
    EXPECT_EQ(static_cast<uint64_t>(last_seq), evs[1].seq);
    EXPECT_GT(evs[0].ts_ns, 0u);

    // delete 事件（主键交付）
    ASSERT_TRUE(srv.engine()->DeleteByKey(t.Value(), rtdb::ColumnValue::Int(1)).IsOk());
    ASSERT_EQ(FetchDelta(c, sub_id, static_cast<uint64_t>(last_seq), 2000, 1u << 20, &out),
              Err::kOk);
    ASSERT_TRUE(ParseFetchOut(out, &last_seq, &evs));
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].kind, RTDB_CHG_DELETE);
    EXPECT_EQ(evs[0].key.as_int(), 1);

    // 追平后空拉取：0 变更、last_seq 不回退
    ASSERT_EQ(FetchDelta(c, sub_id, static_cast<uint64_t>(last_seq), 0, 1u << 20, &out),
              Err::kOk);
    int64_t last2 = 0;
    ASSERT_TRUE(ParseFetchOut(out, &last2, &evs));
    EXPECT_EQ(evs.size(), 0u);
    EXPECT_EQ(last2, last_seq);

    // 行已墓碑后回扫更早版本：变长列一律 NULL（版本门显式语义）
    ASSERT_EQ(FetchDelta(c, sub_id, static_cast<uint64_t>(start_seq), 0, 1u << 20, &out),
              Err::kOk);
    ASSERT_TRUE(ParseFetchOut(out, &last2, &evs));
    ASSERT_EQ(evs.size(), 3u);
    EXPECT_TRUE(evs[0].row[2].is_null());
    EXPECT_TRUE(evs[1].row[2].is_null());

    // ACK（诊断账本）
    ASSERT_EQ(Ack(c, sub_id, static_cast<uint64_t>(last_seq)), Err::kOk);
    // 错误订阅号 → 403
    EXPECT_EQ(FetchDelta(c, sub_id + 1, 0, 0, 1u << 20, nullptr), Err::kProtocolError);
    EXPECT_EQ(Ack(c, sub_id + 1, 0), Err::kProtocolError);

    // UNREGISTER 后 FETCH → 403
    ASSERT_EQ(Unregister(c, sub_id), Err::kOk);
    EXPECT_EQ(FetchDelta(c, sub_id, 0, 0, 1u << 20, nullptr), Err::kProtocolError);

    // 未订阅时 FETCH → 403
    EXPECT_EQ(FetchDelta(c, 1, 0, 0, 1u << 20, nullptr), Err::kProtocolError);
}

TEST_F(ServerTest, FetchDeltaWakesOnWrite) {
    Client c = ConnectAuthed();
    ASSERT_EQ(CreateViaDdl(c, MakeDef2("t_wake")), Err::kOk);
    auto t = srv.engine()->OpenTable("t_wake");
    ASSERT_TRUE(t.IsOk());

    uint32_t sub_id = 0;
    int64_t start_seq = 0;
    ASSERT_EQ(Subscribe(c, {"t_wake"}, &sub_id, &start_seq), Err::kOk);

    // 长轮询等待中并发写入：应在写后很快交付（远小于 timeout 预算）。
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        (void)srv.engine()->Insert(
            t.Value(), {rtdb::ColumnValue::Int(7), rtdb::ColumnValue::Int(70)});
    });
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> out;
    ASSERT_EQ(FetchDelta(c, sub_id, static_cast<uint64_t>(start_seq), 5000, 1u << 20, &out),
              Err::kOk);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    writer.join();

    int64_t last_seq = 0;
    std::vector<ParsedChange> evs;
    ASSERT_TRUE(ParseFetchOut(out, &last_seq, &evs));
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].row[0].as_int(), 7);
    EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST_F(ServerTest, FetchDeltaTruncatesByMaxBytes) {
    Client c = ConnectAuthed();
    ASSERT_EQ(CreateViaDdl(c, MakeDef2("t_slow")), Err::kOk);

    // 先订阅（游标初值 = 订阅时 committed），再写入 20 行。
    uint32_t sub_id = 0;
    int64_t start_seq = 0;
    ASSERT_EQ(Subscribe(c, {"t_slow"}, &sub_id, &start_seq), Err::kOk);
    SeedRows("t_slow", 20);

    // 逐次以 200B 预算拉取，直到追平；总交付 = 20 且无重复（seq 严格递增）。
    uint64_t from = static_cast<uint64_t>(start_seq);
    int total = 0;
    int fetches = 0;
    uint64_t prev_seq = 0;
    bool first = true;
    while (fetches < 100) {
        std::vector<uint8_t> out;
        ASSERT_EQ(FetchDelta(c, sub_id, from, 0, 200, &out), Err::kOk);
        int64_t last_seq = 0;
        std::vector<ParsedChange> evs;
        ASSERT_TRUE(ParseFetchOut(out, &last_seq, &evs));
        if (evs.empty()) break;
        for (const ParsedChange& e : evs) {
            if (!first) EXPECT_GT(e.seq, prev_seq);  // 严格递增，无重复
            prev_seq = e.seq;
            first = false;
            EXPECT_EQ(e.row[0].as_int(), static_cast<int64_t>(total + 1));
            ++total;
        }
        from = static_cast<uint64_t>(last_seq);
        ++fetches;
    }
    EXPECT_EQ(total, 20);
    EXPECT_GT(fetches, 1);  // 200B 预算下确实发生了截断分批
}

TEST_F(ServerTest, FetchDeltaOverflowThenResync) {
    Client c = ConnectAuthed();
    rtdb::TableDef def = MakeDef2("t_of");
    def.row_cap = 2048;  // 三批共 1800 行（环 64KB，三批必然溢出）
    ASSERT_EQ(CreateViaDdl(c, def), Err::kOk);

    uint32_t sub_id = 0;
    int64_t start_seq = 0;
    ASSERT_EQ(Subscribe(c, {"t_of"}, &sub_id, &start_seq), Err::kOk);

    auto t = srv.engine()->OpenTable("t_of");
    ASSERT_TRUE(t.IsOk());
    for (int b = 0; b < 3; ++b) {
        std::vector<rtdb::Engine::BatchOp> ops;
        ops.reserve(600);
        for (int i = 0; i < 600; ++i) {
            rtdb::Engine::BatchOp op;
            op.type = rtdb::Engine::BatchOp::Type::kInsert;
            op.table = t.Value();
            op.row = {rtdb::ColumnValue::Int(b * 600 + i), rtdb::ColumnValue::Int(i)};
            ops.push_back(std::move(op));
        }
        ASSERT_TRUE(srv.engine()->ExecuteBatch(ops).IsOk());
    }

    // 游标起点已被环回收越出窗口 → 402
    EXPECT_EQ(FetchDelta(c, sub_id, static_cast<uint64_t>(start_seq), 0, 1u << 20, nullptr),
              Err::kCursorOverflow);

    // 重同步：从当前 committed 重订阅起点拉取 → 空批
    const int64_t committed = srv.engine()->CommittedSeq();
    ASSERT_GT(committed, 0);
    std::vector<uint8_t> out;
    ASSERT_EQ(FetchDelta(c, sub_id, static_cast<uint64_t>(committed), 0, 1u << 20, &out),
              Err::kOk);
    int64_t last_seq = 0;
    std::vector<ParsedChange> evs;
    ASSERT_TRUE(ParseFetchOut(out, &last_seq, &evs));
    EXPECT_EQ(evs.size(), 0u);
    EXPECT_EQ(last_seq, committed);
}

// ---------------- 会话上限 / TTL ----------------

class SmallSessionsTest : public ServerTest {
protected:
    void Configure(rtdb::server::Config& cfg) override { cfg.max_sessions = 1; }
};

TEST_F(SmallSessionsTest, OverCapRejectedWith404) {
    Client a;
    ASSERT_TRUE(a.Connect(port));
    ASSERT_EQ(a.Auth(kSalt, kUser, kPassword), Err::kOk);
    EXPECT_EQ(a.Call(RTDB_PF_PING, {}, nullptr), Err::kOk);  // 确认已登记

    Client b;
    ASSERT_TRUE(b.Connect(port));
    EXPECT_EQ(b.Call(RTDB_PF_PING, {}, nullptr), Err::kTooManySessions);  // 404
    EXPECT_EQ(b.Call(RTDB_PF_PING, {}, nullptr), Err::kNetworkError);     // 之后被关闭
    EXPECT_EQ(a.Call(RTDB_PF_PING, {}, nullptr), Err::kOk);               // 旧会话不受影响
    EXPECT_GE(srv.stats().rejected_full, 1u);

    // 释放名额后新连接可进入（异步收敛，轮询至多 ~3s）
    a.Close();
    bool admitted = false;
    for (int i = 0; i < 60 && !admitted; ++i) {
        Client c2;
        if (c2.Connect(port) && c2.Auth(kSalt, kUser, kPassword) == Err::kOk) {
            admitted = rtdb::IsOk(c2.Call(RTDB_PF_PING, {}, nullptr));
        }
        if (!admitted) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(admitted);
}

class ShortTtlTest : public ServerTest {
protected:
    void Configure(rtdb::server::Config& cfg) override { cfg.session_ttl_ms = 500; }
};

TEST_F(ShortTtlTest, SessionExpiresThenReauth) {
    Client c;
    ASSERT_TRUE(c.Connect(port));
    ASSERT_EQ(c.Auth(kSalt, kUser, kPassword), Err::kOk);
    EXPECT_EQ(c.Call(RTDB_PF_PING, {}, nullptr), Err::kOk);

    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    EXPECT_EQ(c.Call(RTDB_PF_PING, {}, nullptr), Err::kTokenExpired);  // 502

    // 重新认证即恢复
    ASSERT_EQ(c.Auth(kSalt, kUser, kPassword), Err::kOk);
    EXPECT_EQ(c.Call(RTDB_PF_PING, {}, nullptr), Err::kOk);
}

// ---------------- 优雅退出 ----------------

TEST_F(ServerTest, GracefulStopFlushesAndClosesSessions) {
    Client c = ConnectAuthed();
    ASSERT_EQ(CreateViaDdl(c, MakeDef2("t_stop")), Err::kOk);
    std::vector<OpSpec> ops;
    ops.push_back({1, "t_stop", {rtdb::ColumnValue::Int(1), rtdb::ColumnValue::Int(99)}});
    ASSERT_EQ(c.Call(RTDB_PF_EXEC_BATCH, OpsBody(ops), nullptr), Err::kOk);

    srv.Stop();
    EXPECT_FALSE(srv.running());
    EXPECT_EQ(srv.engine(), nullptr);
    EXPECT_EQ(c.Call(RTDB_PF_PING, {}, nullptr), Err::kNetworkError);  // 服务端已关闭连接

    // 关闭引擎已最终 flush：重开映像数据在
    rtdb::Options o;
    o.instance = "srv_reopen_stop";
    o.path = path;
    o.role = rtdb::Role::Standalone;
    auto eng = rtdb::Engine::Open(o);
    ASSERT_TRUE(eng.IsOk());
    auto t = eng.Value()->OpenTable("t_stop");
    ASSERT_TRUE(t.IsOk());
    auto row = eng.Value()->GetByKey(t.Value(), rtdb::ColumnValue::Int(1));
    ASSERT_TRUE(row.IsOk());
    EXPECT_EQ(row.Value()[1].as_int(), 99);
}

// ---------------- 配置解析 ----------------

TEST(ServerConfig, ParseTextListenAndValidation) {
    const std::string digest =
        rtdb::server::DigestHex(rtdb::server::HashPassword("salty", "pw"));
    const std::string text =
        "# rtdbd 测试配置\n"
        "instance = srv1\n"
        "path = d:/data/x.rtdb\n"
        "listen = 0.0.0.0:7878\n"
        "size_mb = 128\n"
        "fsync = always\n"
        "auth_salt = salty\n"
        "user = admin " + digest + "\n"
        "session_ttl_ms = 1000\n"
        "max_sessions = 8\n"
        "snapshot_on_exit = true\n";
    auto r = rtdb::server::ParseConfigText(text);
    ASSERT_TRUE(r.IsOk());
    const rtdb::server::Config& cfg = r.Value();
    EXPECT_EQ(cfg.instance, "srv1");
    EXPECT_EQ(cfg.path, "d:/data/x.rtdb");
    EXPECT_EQ(cfg.listen_host, "0.0.0.0");
    EXPECT_EQ(cfg.listen_port, 7878);
    EXPECT_EQ(cfg.initial_size_mb, 128u);
    EXPECT_EQ(cfg.fsync, rtdb::FsyncPolicy::Always);
    EXPECT_EQ(cfg.auth_salt, "salty");
    ASSERT_EQ(cfg.users.size(), 1u);
    EXPECT_EQ(cfg.users[0].name, "admin");
    EXPECT_EQ(rtdb::server::DigestHex(cfg.users[0].digest), digest);
    EXPECT_EQ(cfg.session_ttl_ms, 1000u);
    EXPECT_EQ(cfg.max_sessions, 8u);
    EXPECT_TRUE(cfg.snapshot_on_exit);

    // 未知键/畸形行拒绝
    EXPECT_FALSE(rtdb::server::ParseConfigText("bogus = 1").IsOk());
    EXPECT_FALSE(rtdb::server::ParseConfigText("user = admin zz").IsOk());
    EXPECT_FALSE(rtdb::server::ParseConfigText("listen = nope").IsOk());
    EXPECT_FALSE(rtdb::server::ParseConfigText("size_mb = 0").IsOk());

    std::string host;
    uint16_t port = 0;
    ASSERT_TRUE(rtdb::IsOk(rtdb::server::ParseListen("127.0.0.1:5599", &host, &port)));
    EXPECT_EQ(host, "127.0.0.1");
    EXPECT_EQ(port, 5599);
    ASSERT_TRUE(rtdb::IsOk(rtdb::server::ParseListen("[::1]:80", &host, &port)));
    EXPECT_EQ(host, "::1");
    EXPECT_EQ(port, 80);
    EXPECT_FALSE(rtdb::IsOk(rtdb::server::ParseListen("nohost", &host, &port)));
    EXPECT_FALSE(rtdb::IsOk(rtdb::server::ParseListen("h:99999", &host, &port)));
}

// ---------------- rtdbd 拉起烟测 ----------------

#if defined(RTDBD_EXE_PATH)
TEST(ServerSpawnSmoke, RtdbdBinaryServesAndExitsCleanly) {
    // 端口选取：临时绑定 :0 取回后释放（子进程不支持"报告端口"，故先取）。
    uint16_t port = 0;
    {
        auto tmp = rtdb::platform::TcpListener::Listen("127.0.0.1", 0, 4);
        ASSERT_TRUE(tmp.IsOk());
        port = tmp.Value().port();
        tmp.Value().Close();
    }
    ASSERT_NE(port, 0);

    const std::string path = "rtdb_gtest_rtdbd_smoke.rtdb";
    std::error_code ec;
    fs::remove(path, ec);

    const std::string salt = "smoke-salt";
    const std::string digest = rtdb::server::DigestHex(
        rtdb::server::HashPassword(salt, "smoke-pw"));

    std::vector<std::string> args;
    args.push_back("--path");
    args.push_back(path);
    args.push_back("--instance");
    args.push_back("rtdbd_smoke");
    args.push_back("--listen");
    args.push_back("127.0.0.1:" + std::to_string(port));
    args.push_back("--auth-salt");
    args.push_back(salt);
    args.push_back("--user");
    args.push_back("smoke=" + digest);
    args.push_back("--exit-after-ms");
    args.push_back("4000");

    rtdb_test::SpawnOutcome outcome;
    std::thread child([&] { outcome = rtdb_test::SpawnSelf(RTDBD_EXE_PATH, args); });

    // 就绪轮询：连接 + AUTH + PING（至多 ~6s）
    bool served = false;
    for (int i = 0; i < 120 && !served; ++i) {
        Client c;
        if (c.Connect(port)) {
            uint32_t ttl = 0;
            if (c.Auth(salt, "smoke", "smoke-pw", &ttl) == Err::kOk) {
                std::vector<uint8_t> out;
                if (c.Call(RTDB_PF_PING, {}, &out) == Err::kOk) served = true;
            }
        }
        if (!served) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(served) << "rtdbd 未在预算内就绪";
    child.join();
    EXPECT_TRUE(outcome.spawned);
    EXPECT_EQ(outcome.exit_code, 0);  // --exit-after-ms 优雅退出

    fs::remove(path, ec);
}
#endif  // RTDBD_EXE_PATH
