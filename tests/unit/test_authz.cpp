/// \file
/// \brief M4 Authz/PerfCounters 集成测试（docs/16 D1~D3）：
///  - 配置权限规约解析（legacy admin/显式 r/w/ddl/通配/非法拒绝）；
///  - 表级 r/w、DDL、管理面权限门与 kPermissionDenied(503) + 审计计数；
///  - AUTHZ_ME 授权摘要与 ADMIN/STATS 监控计数（admin 专属）；
///  - M3 旧配置（无权限规约）默认 admin 的兼容语义。

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "rtdb/rtdb_client.hpp"
#include "server/authz.hpp"
#include "server/config.hpp"
#include "server/server.hpp"

namespace fs = std::filesystem;

namespace {

using rtdb::Err;

constexpr const char* kSalt = "authz-salt";
constexpr const char* kAdmin = "admin";
constexpr const char* kAdminPw = "admin-pass";
constexpr const char* kReader = "reader";
constexpr const char* kReaderPw = "reader-pass";
constexpr const char* kWriter = "writer";
constexpr const char* kWriterPw = "writer-pass";
constexpr const char* kDdl = "ddl";
constexpr const char* kDdlPw = "ddl-pass";
constexpr const char* kLegacy = "legacy";
constexpr const char* kLegacyPw = "legacy-pass";
constexpr const char* kTable = "auth_t";

rtdb::TableDef MakeDef(const std::string& name) {
    rtdb::TableDef def;
    def.name = name;
    def.columns.push_back({"id", rtdb::DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", rtdb::DataType::kInt64, 0, true, false});
    return def;
}

/// 进程内 Server + 多用户权限表；admin 在 SetUp 建好 kTable。
class AuthzTest : public ::testing::Test {
protected:
    rtdb::server::Server srv;
    rtdb::server::Config cfg;
    std::string path;
    uint16_t port = 0;

    void AddUser(const std::string& name, const std::string& password,
                 const std::string& perm_spec) {
        rtdb::server::UserEntry u;
        u.name = name;
        u.digest = rtdb::server::HashPassword(cfg.auth_salt, password);
        EXPECT_TRUE(rtdb::IsOk(rtdb::server::ParsePermissions(perm_spec, &u.perms)))
            << "权限规约非法：" << perm_spec;
        cfg.users.push_back(std::move(u));
    }

    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag = info->name();
        path = "rtdb_gtest_authz_" + tag + ".rtdb";
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path + ".snap", ec);

        cfg.instance = "authz_" + tag;
        cfg.path = path;
        cfg.initial_size_mb = 32;
        cfg.listen_host = "127.0.0.1";
        cfg.listen_port = 0;  // 系统分配
        cfg.fsync = rtdb::FsyncPolicy::Never;
        cfg.auth_salt = kSalt;
        AddUser(kAdmin, kAdminPw, "admin");
        AddUser(kReader, kReaderPw, "r:" + std::string(kTable));
        AddUser(kWriter, kWriterPw, "w:" + std::string(kTable));
        AddUser(kDdl, kDdlPw, "ddl r:" + std::string(kTable));
        AddUser(kLegacy, kLegacyPw, "");  // M3 兼容：默认 admin

        ASSERT_TRUE(rtdb::IsOk(srv.Start(cfg))) << srv.last_error();
        port = srv.port();
        ASSERT_NE(port, 0);

        std::unique_ptr<rtdb::Client> admin = Connect(kAdmin, kAdminPw);
        ASSERT_TRUE(admin != nullptr);
        ASSERT_TRUE(admin->CreateTable(MakeDef(kTable)).IsOk());
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
        EXPECT_TRUE(r.IsOk()) << "连接失败（" << user << "）：" << rtdb::ErrorMessage(r.Error());
        if (!r.IsOk()) return nullptr;
        return r.TakeValue();
    }

    static std::vector<rtdb::ClientBatchOp> OneInsert(int64_t id, int64_t val) {
        rtdb::ClientBatchOp op;
        op.type = rtdb::ClientBatchOp::Type::kInsert;
        op.table = kTable;
        op.row = {rtdb::ColumnValue::Int(id), rtdb::ColumnValue::Int(val)};
        return {op};
    }
};

// ---------------- 配置/权限规约解析 ----------------

TEST(AuthzConfigTest, LegacyEmptySpecIsAdmin) {
    rtdb::server::UserPerms p;
    ASSERT_TRUE(rtdb::IsOk(rtdb::server::ParsePermissions("", &p)));
    EXPECT_TRUE(p.admin);
    EXPECT_TRUE(p.is_legacy);
    EXPECT_TRUE(p.read_tables.empty());
    EXPECT_TRUE(p.write_tables.empty());
}

TEST(AuthzConfigTest, ExplicitSpecParsesFlagsAndTableLists) {
    rtdb::server::UserPerms p;
    ASSERT_TRUE(rtdb::IsOk(rtdb::server::ParsePermissions("ddl r:meters,alarms w:pre*", &p)));
    EXPECT_FALSE(p.admin);
    EXPECT_FALSE(p.is_legacy);
    EXPECT_TRUE(p.ddl);
    ASSERT_EQ(p.read_tables.size(), 2u);
    EXPECT_EQ(p.read_tables[0], "meters");
    EXPECT_EQ(p.read_tables[1], "alarms");
    ASSERT_EQ(p.write_tables.size(), 1u);
    EXPECT_EQ(p.write_tables[0], "pre*");

    rtdb::server::UserPerms rw;
    ASSERT_TRUE(rtdb::IsOk(rtdb::server::ParsePermissions("rw:*", &rw)));
    EXPECT_EQ(rw.read_tables.size(), 1u);
    EXPECT_EQ(rw.write_tables.size(), 1u);
    EXPECT_EQ(rw.read_tables[0], "*");
}

TEST(AuthzConfigTest, InvalidSpecRejected) {
    rtdb::server::UserPerms p;
    EXPECT_EQ(rtdb::server::ParsePermissions("r", &p), Err::kInvalidArgument);
    EXPECT_EQ(rtdb::server::ParsePermissions("r:", &p), Err::kInvalidArgument);
    EXPECT_EQ(rtdb::server::ParsePermissions("x:y", &p), Err::kInvalidArgument);
    EXPECT_EQ(rtdb::server::ParsePermissions("r:a b:", &p), Err::kInvalidArgument);
}

TEST(AuthzConfigTest, ConfigTextUserLineWithPermissions) {
    const std::string hex = rtdb::server::DigestHex(
        rtdb::server::HashPassword(kSalt, "pw"));
    const std::string text =
        "instance = t\npath = t.rtdb\n"
        "user = alice " + hex + " rw:t1,t2\n"
        "user = bob " + hex + "\n";
    auto c = rtdb::server::ParseConfigText(text);
    ASSERT_TRUE(c.IsOk()) << rtdb::ErrorMessage(c.Error());
    const auto& users = c.Value().users;
    ASSERT_EQ(users.size(), 2u);
    EXPECT_EQ(users[0].name, "alice");
    EXPECT_FALSE(users[0].perms.admin);
    EXPECT_FALSE(users[0].perms.is_legacy);
    ASSERT_EQ(users[0].perms.read_tables.size(), 2u);
    EXPECT_EQ(users[0].perms.write_tables.size(), 2u);
    EXPECT_EQ(users[1].name, "bob");
    EXPECT_TRUE(users[1].perms.admin);  // legacy 兼容
    EXPECT_TRUE(users[1].perms.is_legacy);
}

TEST(AuthzConfigTest, TablePatternMatching) {
    EXPECT_TRUE(rtdb::server::TablePatternMatches("*", "any"));
    EXPECT_TRUE(rtdb::server::TablePatternMatches("meter*", "meters"));
    EXPECT_TRUE(rtdb::server::TablePatternMatches("meters", "meters"));
    EXPECT_FALSE(rtdb::server::TablePatternMatches("meter*", "alarm"));
    EXPECT_FALSE(rtdb::server::TablePatternMatches("meters", "meters2"));
    const std::vector<std::string> list = {"a", "pre*"};
    EXPECT_TRUE(rtdb::server::TableListAllows(list, "a"));
    EXPECT_TRUE(rtdb::server::TableListAllows(list, "prefix"));
    EXPECT_FALSE(rtdb::server::TableListAllows(list, "b"));
}

// ---------------- 服务端权限门（集成） ----------------

TEST_F(AuthzTest, ReaderCanReadButCannotWrite) {
    std::unique_ptr<rtdb::Client> reader = Connect(kReader, kReaderPw);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->Ping().IsOk());

    auto scan = reader->Scan(kTable);
    ASSERT_TRUE(scan.IsOk()) << rtdb::ErrorMessage(scan.Error());
    EXPECT_TRUE(scan.Value().rows.empty());

    const auto w = reader->ExecuteBatch(OneInsert(1, 10));
    ASSERT_FALSE(w.IsOk());
    EXPECT_EQ(w.Error(), Err::kPermissionDenied);

    const rtdb::server::Server::Stats st = srv.stats();
    EXPECT_GE(st.authz_denied, 1u);
    // 拒绝的写必须零副作用（admin 视角仍为空表）。
    std::unique_ptr<rtdb::Client> admin = Connect(kAdmin, kAdminPw);
    ASSERT_TRUE(admin != nullptr);
    auto scan2 = admin->Scan(kTable);
    ASSERT_TRUE(scan2.IsOk());
    EXPECT_TRUE(scan2.Value().rows.empty());
}

TEST_F(AuthzTest, WriterCanWriteButCannotRead) {
    std::unique_ptr<rtdb::Client> writer = Connect(kWriter, kWriterPw);
    ASSERT_TRUE(writer != nullptr);
    ASSERT_TRUE(writer->ExecuteBatch(OneInsert(1, 10)).IsOk());

    const auto r = writer->Scan(kTable);
    ASSERT_FALSE(r.IsOk());
    EXPECT_EQ(r.Error(), Err::kPermissionDenied);
}

TEST_F(AuthzTest, DdlPermissionSeparatedFromReadWrite) {
    std::unique_ptr<rtdb::Client> writer = Connect(kWriter, kWriterPw);
    ASSERT_TRUE(writer != nullptr);
    const auto denied = writer->CreateTable(MakeDef("denied_t"));
    ASSERT_FALSE(denied.IsOk());
    EXPECT_EQ(denied.Error(), Err::kPermissionDenied);

    std::unique_ptr<rtdb::Client> ddl = Connect(kDdl, kDdlPw);
    ASSERT_TRUE(ddl != nullptr);
    ASSERT_TRUE(ddl->CreateTable(MakeDef("ddl_t")).IsOk());
    // describe = 元数据读：ddl 用户可读任意表元数据（docs/16 D2）。
    ASSERT_TRUE(ddl->DescribeTable("ddl_t").IsOk());

    // reader 对未授权表不可 describe（无 r/w、非 ddl）。
    std::unique_ptr<rtdb::Client> reader = Connect(kReader, kReaderPw);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->DescribeTable(kTable).IsOk());
    const auto d = reader->DescribeTable("ddl_t");
    ASSERT_FALSE(d.IsOk());
    EXPECT_EQ(d.Error(), Err::kPermissionDenied);
}

TEST_F(AuthzTest, AdminOnlyStatsAndAuthzMe) {
    std::unique_ptr<rtdb::Client> reader = Connect(kReader, kReaderPw);
    ASSERT_TRUE(reader != nullptr);
    const auto denied = reader->GetServerStats();
    ASSERT_FALSE(denied.IsOk());
    EXPECT_EQ(denied.Error(), Err::kPermissionDenied);

    auto me = reader->GetAuthzInfo();
    ASSERT_TRUE(me.IsOk()) << rtdb::ErrorMessage(me.Error());
    EXPECT_EQ(me.Value().user, kReader);
    EXPECT_FALSE(me.Value().admin);
    EXPECT_FALSE(me.Value().ddl);
    ASSERT_EQ(me.Value().read_tables.size(), 1u);
    EXPECT_EQ(me.Value().read_tables[0], kTable);

    std::unique_ptr<rtdb::Client> admin = Connect(kAdmin, kAdminPw);
    ASSERT_TRUE(admin != nullptr);
    auto st = admin->GetServerStats();
    ASSERT_TRUE(st.IsOk()) << rtdb::ErrorMessage(st.Error());
    EXPECT_TRUE(st.Value().Has("requests"));
    EXPECT_TRUE(st.Value().Has("authz_denied"));
    EXPECT_TRUE(st.Value().Has("sessions_current"));
    EXPECT_TRUE(st.Value().Has("uptime_ms"));
    EXPECT_TRUE(st.Value().Has("session_ttl_ms"));
    EXPECT_GE(st.Value().Get("requests"), 1u);
    EXPECT_GE(st.Value().Get("sessions_current"), 1u);
    EXPECT_GE(st.Value().Get("admin_requests"), 1u);

    auto admin_me = admin->GetAuthzInfo();
    ASSERT_TRUE(admin_me.IsOk());
    EXPECT_TRUE(admin_me.Value().admin);
}

TEST_F(AuthzTest, LegacyUserDefaultsToAdmin) {
    std::unique_ptr<rtdb::Client> legacy = Connect(kLegacy, kLegacyPw);
    ASSERT_TRUE(legacy != nullptr);
    ASSERT_TRUE(legacy->CreateTable(MakeDef("legacy_t")).IsOk());
    ASSERT_TRUE(legacy->ExecuteBatch(OneInsert(1, 1)).IsOk());
    auto st = legacy->GetServerStats();  // legacy admin 也允许管理面
    ASSERT_TRUE(st.IsOk()) << rtdb::ErrorMessage(st.Error());
}

}  // namespace
