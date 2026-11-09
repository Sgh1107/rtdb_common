/// \file
/// \brief M4 兼容与版本纪律用例（docs/16 D11 / §6 兼容测试，约 4 例）：
///  - C ABI v2 单一真源（RTDB_ABI_VERSION / RTDB_ABI_VER）编译期 + 运行期
///    校验，v2 订阅族符号链接与空句柄边界；
///  - 旧客户端（M3-W4 rtdb_client_c.h，仅旧 func）连新服务端：legacy 用户
///    （无权限规约）默认 admin，建表/块写/扫描/PING 与管理面均放行；
///  - 新客户端连旧服务端：以协议层模拟 M3 服务端（真实旧二进制不可得），
///    新增 func（ADMIN=40 / AUTHZ_ME=41）统一降级 kNotImplemented(400)，
///    不崩溃、不破坏既有会话；
///  - 独立映像文件：shm 布局版本冻结、C ABI v1 建表/插入/扫描语义不变、
///    v2 订阅族只增符号在同一映像可用。
///
/// 每用例独立映像文件 + SetUp/TearDown 清理（参考 test_sql_exec.cpp 与
/// test_authz.cpp）；不依赖任何 CMake 新宏。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// proto_v1.h 的 RTDB_CHG_* 无 #ifndef 保护，必须先于 rtdb_cabi.h
// （后者对同名宏做 #ifndef 保护），否则宏重定义告警。
#include "rtdb/proto_v1.h"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "rtdb/rtdb_cabi.h"
#include "rtdb/rtdb_client.hpp"
#include "rtdb/rtdb_client_c.h"
#include "rtdb/version.hpp"
#include "net/codec.hpp"
#include "net/wire.hpp"
#include "platform/net.hpp"
#include "server/authz.hpp"
#include "server/config.hpp"
#include "server/server.hpp"

namespace fs = std::filesystem;

namespace {

using rtdb::Err;

constexpr const char* kSalt = "compat-salt";
constexpr const char* kLegacy = "legacy";
constexpr const char* kLegacyPw = "legacy-pass";
constexpr const char* kTable = "compat_t";

// ---- 编译期单一真源校验（与 src/api/c_abi.cpp 的 static_assert 呼应）----
static_assert(RTDB_ABI_VERSION == RTDB_ABI_VER,
              "RTDB_ABI_VERSION 与 RTDB_ABI_VER 必须一致（C ABI v2 单一真源）");
static_assert(RTDB_ABI_VERSION == 2, "M4 冻结 C ABI v2：不新增符号、不改既有签名");
static_assert(rtdb::kShmFormatVersion == 2,
              "M4 不改 shm 布局（PerfCounters 走 host + RPC）");
static_assert(RTDB_ERR_NOT_IMPLEMENTED == RTDB_E_NOT_IMPLEMENTED,
              "C ABI 与协议层的 NOT_IMPLEMENTED(400) 必须对齐");
static_assert(RTDB_PF_ADMIN == 40u && RTDB_PF_AUTHZ_ME == 41u,
              "M4 新增 func 只增不改：ADMIN=40 / AUTHZ_ME=41");

// ---------------- 1. ABI 版本 / v2 符号 ----------------

TEST(CompatAbiTest, VersionSingleSourceAndV2SymbolsLinked) {
    // 运行期：库内 rtdb_abi_version() 与两个宏一致（头文件判版 vs 实际符号集）。
    EXPECT_EQ(rtdb_abi_version(), static_cast<uint32_t>(RTDB_ABI_VER));
    EXPECT_EQ(static_cast<int>(rtdb_abi_version()), RTDB_ABI_VERSION);
    EXPECT_EQ(static_cast<int>(Err::kNotImplemented), RTDB_ERR_NOT_IMPLEMENTED);
    EXPECT_EQ(RTDB_ERR_NOT_IMPLEMENTED, 400);

    // v2 订阅族「只增符号」：链接期存在 + 空句柄边界安全（不得崩溃）。
    EXPECT_EQ(rtdb_abi_subscribe(nullptr, nullptr, 0, nullptr),
              RTDB_ERR_INVALID_ARGUMENT);
    rtdb_batch batch = {};
    EXPECT_EQ(rtdb_abi_cursor_next(nullptr, 0, &batch), RTDB_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(rtdb_abi_cursor_position(nullptr), uint64_t{0});
    rtdb_abi_cursor_reset(nullptr, 0);
    rtdb_abi_cursor_close(nullptr);
    rtdb_abi_free_batch(&batch);
    EXPECT_TRUE(batch.changes == nullptr);
    EXPECT_EQ(batch.nchanges, std::size_t{0});
}

// ---------------- 2. 旧客户端 ↔ 新服务端（进程内 Server）----------------

/// 进程内新服务端 + 唯一 legacy 用户（空权限规约 = 默认 admin）。
/// 每用例按测试名生成独立映像，SetUp/TearDown 清理（同 test_authz.cpp）。
class CompatServerTest : public ::testing::Test {
protected:
    rtdb::server::Server srv;
    rtdb::server::Config cfg;
    std::string path;
    uint16_t port = 0;

    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag = info->name();
        path = "rtdb_gtest_compat_" + tag + ".rtdb";
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path + ".snap", ec);

        cfg.instance = "compat_" + tag;
        cfg.path = path;
        cfg.initial_size_mb = 32;
        cfg.listen_host = "127.0.0.1";
        cfg.listen_port = 0;  // 系统分配
        cfg.fsync = rtdb::FsyncPolicy::Never;
        cfg.auth_salt = kSalt;
        // 唯一用户 = M3 兼容用户：无权限规约 → 服务端按 admin 放行（D11）。
        rtdb::server::UserEntry u;
        u.name = kLegacy;
        u.digest = rtdb::server::HashPassword(cfg.auth_salt, kLegacyPw);
        EXPECT_TRUE(rtdb::IsOk(rtdb::server::ParsePermissions("", &u.perms)))
            << "空权限规约应解析为 legacy admin";
        cfg.users.push_back(std::move(u));

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

    std::unique_ptr<rtdb::Client> Connect(const std::string& user,
                                          const std::string& password) {
        rtdb::ClientOptions o;
        o.host = "127.0.0.1";
        o.port = port;
        o.user = user;
        o.password = password;
        o.auth_salt = kSalt;
        o.connect_timeout_ms = 2000;
        o.request_timeout_ms = 3000;
        auto r = rtdb::Client::Connect(o);
        EXPECT_TRUE(r.IsOk()) << "连接失败（" << user << "）："
                             << rtdb::ErrorMessage(r.Error());
        if (!r.IsOk()) return nullptr;
        return r.TakeValue();
    }
};

TEST_F(CompatServerTest, LegacyClientOldFuncsAndAdminOnNewServer) {
    // 「旧客户端」= M3-W4 rtdb_client_c.h 稳定面：只含 AUTH/PING/DDL/
    // 块写/扫描/订阅，不含 M4 新增的 ADMIN(40)/AUTHZ_ME(41)。
    rtdb_client_options o = {};
    o.host = "127.0.0.1";
    o.port = port;
    o.user = kLegacy;
    o.password = kLegacyPw;
    o.auth_salt = kSalt;
    o.connect_timeout_ms = 2000;
    o.request_timeout_ms = 3000;
    rtdb_client* legacy = nullptr;
    ASSERT_EQ(rtdb_client_connect(&o, &legacy), RTDB_OK);
    ASSERT_NE(legacy, nullptr);

    // 旧 func 1：PING（AUTH 响应与既有 func/payload 不变）。
    int64_t committed = 0;
    uint64_t heartbeat = 0;
    uint32_t ttl_ms = 0;
    EXPECT_EQ(rtdb_client_ping(legacy, &committed, &heartbeat, &ttl_ms), RTDB_OK);

    // 旧 func 2：建表（DDL；legacy 默认 admin 应放行）。
    rtdb_column_def defs[2] = {};
    defs[0].name = "id";
    defs[0].type = RTDB_T_INT64;
    defs[0].nullable = 0;  // 主键列 NOT NULL
    defs[1].name = "val";
    defs[1].type = RTDB_T_INT64;
    defs[1].nullable = 1;
    ASSERT_EQ(rtdb_client_create_table(legacy, kTable, defs, 2, 0), RTDB_OK);

    // 旧 func 3：列式块写（EXEC_BATCH BLOCK 模式）。
    int64_t ids[3] = {1, 2, 3};
    int64_t vals[3] = {10, 20, 30};
    rtdb_column cols[2] = {};
    cols[0].name = "id";
    cols[0].type = RTDB_T_INT64;
    cols[0].data = ids;
    cols[1].name = "val";
    cols[1].type = RTDB_T_INT64;
    cols[1].data = vals;
    uint64_t seq = 0;
    EXPECT_EQ(rtdb_client_upsert_block(legacy, kTable, cols, 2, 3, &seq), RTDB_OK);

    // 旧 func 4：扫描读回。
    rtdb_rows rows = {};
    ASSERT_EQ(rtdb_client_scan(legacy, kTable, nullptr, nullptr, &rows), RTDB_OK);
    EXPECT_EQ(rows.nrows, std::size_t{3});
    rtdb_client_free_rows(&rows);
    rtdb_client_close(legacy);

    // 管理面：新客户端以同一 legacy 用户连接，服务端按 admin 放行
    // ADMIN/STATS 与 AUTHZ_SHOW；AUTHZ_ME 的 legacy 标志位只增不改。
    std::unique_ptr<rtdb::Client> nc = Connect(kLegacy, kLegacyPw);
    ASSERT_NE(nc, nullptr);
    auto st = nc->GetServerStats();
    EXPECT_TRUE(st.IsOk()) << rtdb::ErrorMessage(st.Error());
    auto show = nc->GetAuthzShow();
    EXPECT_TRUE(show.IsOk()) << rtdb::ErrorMessage(show.Error());
    auto me = nc->GetAuthzInfo();
    ASSERT_TRUE(me.IsOk()) << rtdb::ErrorMessage(me.Error());
    EXPECT_TRUE(me.Value().legacy);
    EXPECT_TRUE(me.Value().admin || me.Value().legacy);  // legacy 隐含全权限
    nc->Close();
}

// ---------------- 3. 新客户端 ↔ 旧服务端（协议层模拟）----------------

/// 「旧服务端」协议层模拟（D11：真实 M3 rtdbd 二进制不可得时以协议层模拟）。
/// 行为：AUTH/PING 按 v1 正常应答；其余 func（含 ADMIN=40 / AUTHZ_ME=41）
/// 一律返回 kNotImplemented(400)，模拟 M3 服务端不认识新 func。
/// 模拟边界：只服务首条连接、不实现订阅族与真实认证/存储，仅用于验证
/// 新 func 的降级路径与「不崩溃」；AUTH 凭据不作校验（恒成功）。
class LegacyServerMock {
public:
    LegacyServerMock() = default;
    ~LegacyServerMock() { Stop(); }
    LegacyServerMock(const LegacyServerMock&) = delete;
    LegacyServerMock& operator=(const LegacyServerMock&) = delete;

    bool Start() {
        if (!rtdb::IsOk(rtdb::platform::NetStartup())) return false;
        auto lr = rtdb::platform::TcpListener::Listen("", 0);  // 仅回环 + 系统分配端口
        if (!lr.IsOk()) return false;
        listener_ = lr.TakeValue();
        port_ = listener_.port();
        stop_.store(false);
        thread_ = std::thread([this] { Serve(); });
        return true;
    }

    uint16_t port() const noexcept { return port_; }

    void Stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }

private:
    void Serve() {
        while (!stop_.load()) {
            auto ar = listener_.Accept(200);  // 200ms 轮询，便于 Stop() 收敛
            if (!ar.IsOk()) continue;
            rtdb::platform::TcpSocket sock = ar.TakeValue();
            sock.SetTimeout(3000);
            while (!stop_.load()) {
                rtdb::net::FrameHeader h;
                std::vector<uint8_t> body;
                if (!rtdb::IsOk(rtdb::net::ReadFrame(
                        sock, rtdb::net::kDefaultMaxPayloadBytes, &h, &body))) {
                    break;
                }
                if (!Respond(sock, h)) break;
            }
            break;  // 只服务首条连接（Client 单连接复用）
        }
    }

    static bool Respond(rtdb::platform::TcpSocket& sock,
                        const rtdb::net::FrameHeader& req) {
        std::vector<uint8_t> payload;
        rtdb::net::Writer w(&payload);
        if (req.func == RTDB_PF_AUTH) {
            // v1 AUTH 应答：[u32 session][u32 ttl_ms]（客户端要求恰好读完）。
            rtdb::net::WriteResponsePrologue(w, Err::kOk);
            w.U32(4242u);
            w.U32(60000u);
        } else if (req.func == RTDB_PF_PING) {
            // v1 PING 应答：[i64 seq][u64 hb][i64 checkpoint][u32 ttl]。
            rtdb::net::WriteResponsePrologue(w, Err::kOk);
            w.I64(0);
            w.U64(0);
            w.I64(0);
            w.U32(60000u);
        } else {
            // M3 不认识 M4 新增 func：统一降级 kNotImplemented(400)，
            // 保持连接与会话，不崩溃（D11 兼容要求）。
            rtdb::net::WriteResponsePrologue(w, Err::kNotImplemented,
                                            "legacy server: unsupported func");
        }
        rtdb::net::FrameHeader resp;
        resp.ver = rtdb::net::kProtoVersion;
        resp.flags = rtdb::net::kFlagResp;
        resp.session = req.session;
        resp.func = req.func;
        resp.payload_len = static_cast<uint32_t>(payload.size());
        return rtdb::IsOk(rtdb::net::WriteFrame(
            sock, resp, payload.empty() ? nullptr : payload.data()));
    }

    rtdb::platform::TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    uint16_t port_ = 0;
};

TEST(CompatProtocolTest, NewFuncsDegradeToNotImplementedOnLegacyServer) {
    LegacyServerMock mock;
    ASSERT_TRUE(mock.Start());

    rtdb::ClientOptions o;
    o.host = "127.0.0.1";
    o.port = mock.port();
    o.user = "any";  // 模拟服务端不校验凭据，AUTH 恒成功
    o.password = "any";
    o.auth_salt = "any";
    o.connect_timeout_ms = 2000;
    o.request_timeout_ms = 2000;
    auto cr = rtdb::Client::Connect(o);
    ASSERT_TRUE(cr.IsOk()) << rtdb::ErrorMessage(cr.Error());
    std::unique_ptr<rtdb::Client> cli = cr.TakeValue();

    // 旧 func（PING）不受影响，证明降级只作用于新 func。
    ASSERT_TRUE(cli->Ping().IsOk());

    // 新 func ADMIN(40) / AUTHZ_ME(41) → kNotImplemented，不得崩溃。
    auto st = cli->GetServerStats();
    ASSERT_FALSE(st.IsOk());
    EXPECT_EQ(st.Error(), Err::kNotImplemented);
    auto me = cli->GetAuthzInfo();
    ASSERT_FALSE(me.IsOk());
    EXPECT_EQ(me.Error(), Err::kNotImplemented);

    // 连续调用 + 旧 func 复用同一会话：降级不破坏会话状态。
    EXPECT_EQ(cli->GetAuthzInfo().Error(), Err::kNotImplemented);
    EXPECT_TRUE(cli->Ping().IsOk());
    EXPECT_TRUE(cli->connected());

    cli->Close();
    mock.Stop();
}

// ---------------- 4. 独立映像：布局冻结 + v1 语义 + v2 只增符号 ----------------

class CompatImageTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }
    void TearDown() override { Cleanup(); }

    static void Cleanup() {
        std::error_code ec;
        fs::remove(kImage, ec);
        fs::remove(std::string(kImage) + ".snap", ec);
    }

    static constexpr const char* kImage = "rtdb_gtest_compat.rtdb";
};

TEST_F(CompatImageTest, ShmLayoutFrozenAndV1AbiReadsImageV2SubscribeWorks) {
    // shm 布局版本冻结（D11）：M4 不升级 kShmFormatVersion。
    EXPECT_EQ(rtdb::kShmFormatVersion, uint32_t{2});

    // 用 C ABI v1 符号创建映像（v1 语义不变），再读回。
    rtdb_engine* eng = nullptr;
    ASSERT_EQ(rtdb_abi_open("rtdb_gtest_compat", kImage, RTDB_ROLE_STANDALONE,
                            RTDB_FSYNC_NEVER, &eng),
              RTDB_OK);
    ASSERT_NE(eng, nullptr);

    rtdb_column_def defs[2] = {};
    defs[0].name = "id";
    defs[0].type = RTDB_T_INT64;
    defs[0].nullable = 0;
    defs[1].name = "val";
    defs[1].type = RTDB_T_INT64;
    defs[1].nullable = 1;
    rtdb_table* tbl = nullptr;
    ASSERT_EQ(rtdb_abi_create_table(eng, kTable, defs, 2, 0, &tbl), RTDB_OK);
    ASSERT_NE(tbl, nullptr);

    rtdb_value vals[2] = {};
    vals[0].type = RTDB_T_INT64;
    vals[0].int_value = 1;
    vals[1].type = RTDB_T_INT64;
    vals[1].int_value = 10;
    ASSERT_EQ(rtdb_abi_insert(eng, tbl, vals, 2, nullptr), RTDB_OK);
    rtdb_abi_close_table(tbl);

    // v1 读回：按名打开 + 全表扫描，行数与插入一致（v1 语义未变）。
    rtdb_table* reopened = nullptr;
    ASSERT_EQ(rtdb_abi_open_table(eng, kTable, &reopened), RTDB_OK);
    rtdb_rows rows = {};
    ASSERT_EQ(rtdb_abi_scan(eng, reopened, nullptr, nullptr, &rows), RTDB_OK);
    EXPECT_EQ(rows.nrows, std::size_t{1});
    rtdb_abi_free_rows(&rows);
    rtdb_abi_close_table(reopened);

    // v2 订阅族「只增符号」：在同一 v1 映像上可用（有 WAL 环时 OK，
    // Reader 主角色返回 UNSUPPORTED 亦属合法降级；关键是不得崩溃）。
    rtdb_cursor* cur = nullptr;
    const int32_t sub_rc = rtdb_abi_subscribe(eng, nullptr, 0, &cur);
    EXPECT_TRUE(sub_rc == RTDB_OK || sub_rc == RTDB_ERR_UNSUPPORTED)
        << "rtdb_abi_subscribe 返回 " << sub_rc;
    if (sub_rc == RTDB_OK) {
        ASSERT_NE(cur, nullptr);
        rtdb_batch batch = {};
        EXPECT_EQ(rtdb_abi_cursor_next(cur, 0, &batch), RTDB_OK);
        rtdb_abi_free_batch(&batch);
        rtdb_abi_cursor_reset(cur, 0);
        rtdb_abi_cursor_close(cur);
    }

    rtdb_abi_close(eng);  // debug 断言：所有 table 句柄已先关闭
}

}  // namespace
