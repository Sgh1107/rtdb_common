/// \file
/// \brief M4-W3 SQL 执行器语义单测（docs/16 D5/D6）。
///
/// 覆盖：四类语句、WHERE 布尔/NULL 语义、ORDER BY/LIMIT/OFFSET、
/// 主键下推、单语句原子与零副作用失败、预算保护、错误分类透传、
/// 引擎指标快照。每个用例独立映像，避免跨用例状态耦合。

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "rtdb/column_value.hpp"
#include "rtdb/engine.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "rtdb/result.hpp"
#include "rtdb/sql.hpp"
#include "rtdb/table_def.hpp"

namespace {

namespace fs = std::filesystem;
using rtdb::ColumnDef;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Err;
using rtdb::Options;
using rtdb::Role;
using rtdb::TableDef;
using rtdb::sql::ExecuteOptions;
using rtdb::sql::SqlResult;

class SqlExecTest : public ::testing::Test {
protected:
    void SetUp() override { Cleanup(); }
    void TearDown() override { Cleanup(); }

    static Options MakeOpts() {
        Options opts;
        opts.instance = "sql_exec_test";
        opts.path = kImage;
        opts.initial_size_bytes = 4ull << 20;
        opts.role = Role::Standalone;
        opts.wal_ring_bytes = 1ull << 20;  // 分页用例一次写 600 行，避免整批预留受默认环限制
        return opts;
    }

    static void Cleanup() {
        std::error_code ec;
        fs::remove(kImage, ec);
        fs::remove(std::string(kImage) + ".snap", ec);
    }

    /// id(i64 NOT NULL 主键) / val(f64) / name(var) / flag(bool)。
    static TableDef MakeDef() {
        TableDef def;
        def.name = "meters";
        def.max_rows_hint = 1024;
        def.columns.push_back({"id", DataType::kInt64, 0, false, false});
        def.columns.push_back({"val", DataType::kFloat64, 0, true, false});
        def.columns.push_back({"name", DataType::kUtf8Var, 0, true, false});
        def.columns.push_back({"flag", DataType::kBool, 0, true, false});
        return def;
    }

    std::unique_ptr<rtdb::Engine> OpenWithTable() {
        auto opened = rtdb::Engine::Open(MakeOpts());
        if (!opened.IsOk()) return nullptr;
        auto eng = opened.TakeValue();
        if (!eng->CreateTable(MakeDef()).IsOk()) return nullptr;
        return eng;
    }

    /// 三行种子：id=1(val=1.5,'a',true) / 2(NULL,'b',false) / 3(3.5,NULL,true)。
    static Err Seed(rtdb::Engine& eng) {
        auto r = eng.ExecuteSql(
            "INSERT INTO meters (id, val, name, flag) VALUES "
            "(1, 1.5, 'a', TRUE), (2, NULL, 'b', FALSE), (3, 3.5, NULL, TRUE)");
        return r.IsOk() ? Err::kOk : r.Error();
    }

    static constexpr const char* kImage = "rtdb_gtest_sql_exec.rtdb";
};

TEST_F(SqlExecTest, InsertMultiRowIsAtomicAndSelectAll) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);

    auto r = eng->ExecuteSql("SELECT * FROM meters");
    ASSERT_TRUE(r.IsOk()) << rtdb::ErrorMessage(r.Error());
    const SqlResult& q = r.Value();
    EXPECT_EQ(q.statement, rtdb::sql::StatementType::kSelect);
    ASSERT_EQ(q.columns.size(), 4u);
    EXPECT_EQ(q.columns[0].name, "id");
    EXPECT_EQ(q.columns[0].type, DataType::kInt64);
    EXPECT_EQ(q.columns[2].type, DataType::kUtf8Var);
    ASSERT_EQ(q.rows.size(), 3u);
    EXPECT_EQ(q.rows[0][0].as_int(), 1);
    EXPECT_DOUBLE_EQ(q.rows[0][1].as_real(), 1.5);
    EXPECT_EQ(q.rows[0][2].as_bytes(), "a");
    EXPECT_TRUE(q.rows[1][1].is_null());
    EXPECT_TRUE(q.rows[2][2].is_null());
    EXPECT_GE(q.last_seq, 1);
}

TEST_F(SqlExecTest, InsertReportsAffectedRowsAndTypeCoercion) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    auto r = eng->ExecuteSql("INSERT INTO meters (id, val, name, flag) VALUES "
                             "(7, '2.5', 'seven', 1), (8, 3, 'x', 0)");
    ASSERT_TRUE(r.IsOk()) << rtdb::ErrorMessage(r.Error());
    EXPECT_EQ(r.Value().affected_rows, 2u);
    EXPECT_TRUE(r.Value().rows.empty());
    auto q = eng->ExecuteSql("SELECT val, flag FROM meters WHERE id = 7");
    ASSERT_TRUE(q.IsOk());
    ASSERT_EQ(q.Value().rows.size(), 1u);
    EXPECT_DOUBLE_EQ(q.Value().rows[0][0].as_real(), 2.5);
    EXPECT_TRUE(q.Value().rows[0][1].as_bool());
}

TEST_F(SqlExecTest, SelectProjectionAndAliasKeepsOriginalName) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);
    auto r = eng->ExecuteSql("SELECT name AS label, id FROM meters WHERE id = 2");
    ASSERT_TRUE(r.IsOk()) << rtdb::ErrorMessage(r.Error());
    ASSERT_EQ(r.Value().columns.size(), 2u);
    EXPECT_EQ(r.Value().columns[0].name, "name");  // D4：别名不进入结果列名
    EXPECT_EQ(r.Value().columns[1].name, "id");
    ASSERT_EQ(r.Value().rows.size(), 1u);
    EXPECT_EQ(r.Value().rows[0][0].as_bytes(), "b");
    EXPECT_EQ(r.Value().rows[0][1].as_int(), 2);
}

TEST_F(SqlExecTest, WhereBooleanPrecedenceAndNullSemantics) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);

    auto r = eng->ExecuteSql(
        "SELECT id FROM meters WHERE (id = 1 OR id = 2) AND NOT val = 1.5");
    ASSERT_TRUE(r.IsOk()) << rtdb::ErrorMessage(r.Error());
    ASSERT_EQ(r.Value().rows.size(), 1u);
    EXPECT_EQ(r.Value().rows[0][0].as_int(), 2);

    // SQL 三值逻辑：与 NULL 比较恒为 UNKNOWN（不入选），IS NULL 单独判定。
    auto n = eng->ExecuteSql("SELECT id FROM meters WHERE val = NULL");
    ASSERT_TRUE(n.IsOk());
    EXPECT_TRUE(n.Value().rows.empty());
    auto s = eng->ExecuteSql("SELECT id FROM meters WHERE name IS NULL");
    ASSERT_TRUE(s.IsOk());
    ASSERT_EQ(s.Value().rows.size(), 1u);
    EXPECT_EQ(s.Value().rows[0][0].as_int(), 3);
    auto nn = eng->ExecuteSql("SELECT id FROM meters WHERE name IS NOT NULL");
    ASSERT_TRUE(nn.IsOk());
    EXPECT_EQ(nn.Value().rows.size(), 2u);
}

TEST_F(SqlExecTest, PrimaryKeyRangePushdownKeepsResults) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);
    auto r = eng->ExecuteSql("SELECT id FROM meters WHERE id >= 2 AND id <= 3");
    ASSERT_TRUE(r.IsOk()) << rtdb::ErrorMessage(r.Error());
    ASSERT_EQ(r.Value().rows.size(), 2u);
    EXPECT_EQ(r.Value().rows[0][0].as_int(), 2);
    EXPECT_EQ(r.Value().rows[1][0].as_int(), 3);

    auto eq = eng->ExecuteSql("SELECT id FROM meters WHERE id = 1");
    ASSERT_TRUE(eq.IsOk());
    ASSERT_EQ(eq.Value().rows.size(), 1u);
    EXPECT_EQ(eq.Value().rows[0][0].as_int(), 1);
}

TEST_F(SqlExecTest, OrderByNullsAndLimitOffset) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);
    auto r = eng->ExecuteSql("SELECT id FROM meters ORDER BY val DESC, id ASC");
    ASSERT_TRUE(r.IsOk()) << rtdb::ErrorMessage(r.Error());
    ASSERT_EQ(r.Value().rows.size(), 3u);
    // NULL 约定最小；DESC 下排最后（3.5 → 1.5 → NULL）。
    EXPECT_EQ(r.Value().rows[0][0].as_int(), 3);
    EXPECT_EQ(r.Value().rows[1][0].as_int(), 1);
    EXPECT_EQ(r.Value().rows[2][0].as_int(), 2);

    auto page = eng->ExecuteSql("SELECT id FROM meters ORDER BY val DESC LIMIT 1 OFFSET 1");
    ASSERT_TRUE(page.IsOk());
    ASSERT_EQ(page.Value().rows.size(), 1u);
    EXPECT_EQ(page.Value().rows[0][0].as_int(), 1);

    auto zero = eng->ExecuteSql("SELECT id FROM meters LIMIT 0");
    ASSERT_TRUE(zero.IsOk());
    EXPECT_TRUE(zero.Value().rows.empty());
}

TEST_F(SqlExecTest, LimitWithoutWhereStopsEarly) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);
    auto r = eng->ExecuteSql("SELECT id FROM meters LIMIT 2 OFFSET 1");
    ASSERT_TRUE(r.IsOk());
    ASSERT_EQ(r.Value().rows.size(), 2u);
    EXPECT_EQ(r.Value().rows[0][0].as_int(), 2);
    EXPECT_EQ(r.Value().rows[1][0].as_int(), 3);
}

TEST_F(SqlExecTest, UpdateBatchAtomicAndAffectedRows) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);
    auto u = eng->ExecuteSql("UPDATE meters SET val = 9.5, name = 'z' WHERE id >= 2");
    ASSERT_TRUE(u.IsOk()) << rtdb::ErrorMessage(u.Error());
    EXPECT_EQ(u.Value().affected_rows, 2u);

    auto q = eng->ExecuteSql("SELECT id, val, name FROM meters ORDER BY id");
    ASSERT_TRUE(q.IsOk());
    ASSERT_EQ(q.Value().rows.size(), 3u);
    EXPECT_DOUBLE_EQ(q.Value().rows[0][1].as_real(), 1.5);
    EXPECT_DOUBLE_EQ(q.Value().rows[1][1].as_real(), 9.5);
    EXPECT_EQ(q.Value().rows[1][2].as_bytes(), "z");
    EXPECT_EQ(q.Value().rows[2][2].as_bytes(), "z");
}

TEST_F(SqlExecTest, DeleteWithAndWithoutWhere) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);
    auto d = eng->ExecuteSql("DELETE FROM meters WHERE id = 1");
    ASSERT_TRUE(d.IsOk()) << rtdb::ErrorMessage(d.Error());
    EXPECT_EQ(d.Value().affected_rows, 1u);
    auto a = eng->ExecuteSql("SELECT * FROM meters");
    ASSERT_TRUE(a.IsOk());
    EXPECT_EQ(a.Value().rows.size(), 2u);

    auto all = eng->ExecuteSql("DELETE FROM meters");
    ASSERT_TRUE(all.IsOk());
    EXPECT_EQ(all.Value().affected_rows, 2u);
    auto empty = eng->ExecuteSql("SELECT * FROM meters");
    ASSERT_TRUE(empty.IsOk());
    EXPECT_TRUE(empty.Value().rows.empty());
}

TEST_F(SqlExecTest, FailedBatchHasZeroSideEffects) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);
    // 第二行主键与既有行冲突：ExecuteBatch 校验阶段整批拒绝（WAL 不写 BEGIN）。
    auto r = eng->ExecuteSql("INSERT INTO meters (id, val) VALUES (10, 1.0), (1, 2.0)");
    ASSERT_FALSE(r.IsOk());
    EXPECT_EQ(r.Error(), Err::kAlreadyExists);
    auto q = eng->ExecuteSql("SELECT * FROM meters");
    ASSERT_TRUE(q.IsOk());
    EXPECT_EQ(q.Value().rows.size(), 3u);  // id=10 未写入
}

TEST_F(SqlExecTest, DuplicatePrimaryKeyInsideInsertRejected) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    // M1「批内操作互不依赖」边界：SQL 层显式拒绝，避免两行静默互相覆盖。
    auto r = eng->ExecuteSql("INSERT INTO meters (id, val) VALUES (10, 1.0), (10, 2.0)");
    ASSERT_FALSE(r.IsOk());
    EXPECT_EQ(r.Error(), Err::kAlreadyExists);
    auto q = eng->ExecuteSql("SELECT * FROM meters");
    ASSERT_TRUE(q.IsOk());
    EXPECT_TRUE(q.Value().rows.empty());
}

TEST_F(SqlExecTest, SemanticErrorsAreClassifiedWithDiagnostics) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);

    std::string err;
    EXPECT_EQ(eng->ExecuteSql("SELECT * FROM missing_table", &err).Error(),
              Err::kTableNotFound);
    EXPECT_EQ(eng->ExecuteSql("SELECT nope FROM meters", &err).Error(),
              Err::kInvalidArgument);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("unknown column"), std::string::npos) << err;
    EXPECT_EQ(eng->ExecuteSql("UPDATE meters SET id = 5 WHERE id = 1", &err).Error(),
              Err::kInvalidArgument);
    EXPECT_EQ(eng->ExecuteSql("INSERT INTO meters (val) VALUES (1.0)", &err).Error(),
              Err::kInvalidArgument);
    EXPECT_EQ(eng->ExecuteSql("SELECT * FROM meters WHERE nope = 1", &err).Error(),
              Err::kInvalidArgument);
    EXPECT_EQ(eng->ExecuteSql("UPDATE meters SET name = 5", &err).Error(),
              Err::kInvalidArgument);  // 变长列不接受数值字面量
}

TEST_F(SqlExecTest, ParserErrorClassificationPassesThrough) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    EXPECT_EQ(eng->ExecuteSql("SELECT * FROM a JOIN b ON a.id = b.id").Error(),
              Err::kUnsupported);
    EXPECT_EQ(eng->ExecuteSql("SELECT FROM meters").Error(), Err::kSqlSyntax);
    EXPECT_EQ(eng->ExecuteSql("SELECT count(*) FROM meters").Error(), Err::kUnsupported);
    EXPECT_EQ(eng->ExecuteSql("").Error(), Err::kSqlSyntax);
}

TEST_F(SqlExecTest, RowBudgetProtectsSelectUpdateAndDelete) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(Seed(*eng), Err::kOk);
    ExecuteOptions small;
    small.max_rows = 2;
    EXPECT_EQ(eng->ExecuteSql("SELECT * FROM meters", small).Error(), Err::kLimitExceeded);
    EXPECT_EQ(eng->ExecuteSql("UPDATE meters SET val = 0 WHERE id >= 1", small).Error(),
              Err::kLimitExceeded);
    EXPECT_EQ(eng->ExecuteSql("DELETE FROM meters", small).Error(), Err::kLimitExceeded);
    // 超限语句零副作用。
    auto q = eng->ExecuteSql("SELECT * FROM meters");
    ASSERT_TRUE(q.IsOk());
    EXPECT_EQ(q.Value().rows.size(), 3u);
}

TEST_F(SqlExecTest, FreeFunctionExecuteMatchesEngineApi) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    EXPECT_EQ(rtdb::sql::DefaultMaxRows(), 100000u);
    auto a = rtdb::sql::Execute(*eng, "INSERT INTO meters (id, val) VALUES (1, 1.0)");
    ASSERT_TRUE(a.IsOk()) << rtdb::ErrorMessage(a.Error());
    EXPECT_EQ(a.Value().affected_rows, 1u);
    auto b = eng->ExecuteSql("SELECT id FROM meters");
    ASSERT_TRUE(b.IsOk());
    ASSERT_EQ(b.Value().rows.size(), 1u);
    EXPECT_EQ(b.Value().rows[0][0].as_int(), 1);
}

TEST_F(SqlExecTest, EngineMetricsSnapshotReflectsWritesAndScans) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    const auto before = eng->Metrics();
    ASSERT_EQ(Seed(*eng), Err::kOk);
    const auto after = eng->Metrics();
    EXPECT_GE(after.writes_total, before.writes_total + 3u);
    EXPECT_GE(after.batch_ops, before.batch_ops + 1u);
    EXPECT_GT(after.wal_appended_records, before.wal_appended_records);
    EXPECT_GT(after.wal_appended_bytes, before.wal_appended_bytes);

    auto q = eng->ExecuteSql("SELECT * FROM meters");
    ASSERT_TRUE(q.IsOk());
    const auto scanned = eng->Metrics();
    EXPECT_GT(scanned.scan_calls, after.scan_calls);
    EXPECT_GE(scanned.scan_rows, after.scan_rows + 3u);
    EXPECT_GE(scanned.wal_backlog_bytes, 0u);
}

TEST_F(SqlExecTest, PaginationAcrossMultipleScanPages) {
    auto eng = OpenWithTable();
    ASSERT_NE(eng, nullptr);
    // 600 行 > 单页 512，验证游标续扫不漏行、不重复。
    std::string sql = "INSERT INTO meters (id, val) VALUES ";
    for (int i = 1; i <= 600; ++i) {
        if (i > 1) sql += ", ";
        sql += "(" + std::to_string(i) + ", " + std::to_string(i) + ".0)";
    }
    auto ins = eng->ExecuteSql(sql);
    ASSERT_TRUE(ins.IsOk()) << rtdb::ErrorMessage(ins.Error());
    EXPECT_EQ(ins.Value().affected_rows, 600u);
    auto q = eng->ExecuteSql("SELECT id FROM meters WHERE id >= 1 AND id <= 600");
    ASSERT_TRUE(q.IsOk()) << rtdb::ErrorMessage(q.Error());
    ASSERT_EQ(q.Value().rows.size(), 600u);
    for (size_t i = 0; i < q.Value().rows.size(); ++i)
        EXPECT_EQ(q.Value().rows[i][0].as_int(), static_cast<int64_t>(i + 1));
}

}  // namespace
