/// \file
/// \brief M4 SQL 前端（docs/16 D4）：语法子集解析、AST 形状、错误分类
/// （kSqlSyntax vs kUnsupported）与注释/分号边界。

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "rtdb/sql.hpp"

namespace {

using rtdb::Err;
using rtdb::sql::CompareOp;
using rtdb::sql::Condition;
using rtdb::sql::Parse;
using rtdb::sql::Statement;
using rtdb::sql::StatementType;

TEST(SqlParserTest, SelectStar) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("SELECT * FROM meters", &st, &err), Err::kOk) << err;
    EXPECT_EQ(st.type, StatementType::kSelect);
    EXPECT_TRUE(st.select.select_all);
    EXPECT_TRUE(st.select.columns.empty());
    EXPECT_EQ(st.select.table, "meters");
    EXPECT_FALSE(st.select.where.has_value());
    EXPECT_EQ(st.select.limit, -1);
    EXPECT_EQ(st.select.offset, 0);
}

TEST(SqlParserTest, SelectColumnsAndCaseInsensitiveKeywords) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("  select id, Area ,VAL from meters ", &st, &err), Err::kOk) << err;
    ASSERT_EQ(st.select.columns.size(), 3u);
    EXPECT_EQ(st.select.columns[0], "id");
    EXPECT_EQ(st.select.columns[1], "Area");
    EXPECT_EQ(st.select.columns[2], "VAL");
    EXPECT_EQ(st.select.table, "meters");
}

TEST(SqlParserTest, SelectWithAliasAcceptedButNotStoredInColumns) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("SELECT id AS x FROM t", &st, &err), Err::kOk) << err;
    ASSERT_EQ(st.select.columns.size(), 1u);
    EXPECT_EQ(st.select.columns[0], "id");
}

TEST(SqlParserTest, SelectBooleanPrecedenceAndNot) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("SELECT * FROM t WHERE (a = 1 OR b <> 2) AND NOT c IS NULL", &st, &err),
              Err::kOk)
        << err;
    ASSERT_TRUE(st.select.where.has_value());
    const Condition& root = *st.select.where;
    ASSERT_EQ(root.kind, Condition::Kind::kAnd);
    ASSERT_EQ(root.children.size(), 2u);
    EXPECT_EQ(root.children[0].kind, Condition::Kind::kOr);
    EXPECT_EQ(root.children[0].children.size(), 2u);
    EXPECT_EQ(root.children[0].children[0].column, "a");
    EXPECT_EQ(root.children[0].children[0].op, CompareOp::kEq);
    EXPECT_EQ(root.children[0].children[0].value.as_int(), 1);
    EXPECT_EQ(root.children[0].children[1].column, "b");
    EXPECT_EQ(root.children[0].children[1].op, CompareOp::kNe);
    EXPECT_EQ(root.children[1].kind, Condition::Kind::kNot);
    ASSERT_EQ(root.children[1].children.size(), 1u);
    EXPECT_EQ(root.children[1].children[0].kind, Condition::Kind::kIsNull);
    EXPECT_EQ(root.children[1].children[0].column, "c");
}

TEST(SqlParserTest, SelectAllCompareOperatorsAndLiterals) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("SELECT * FROM t WHERE a = 1 AND b != 2.5 AND c < 'x' AND d <= TRUE"
                    " AND e > -3 AND f >= 4 AND g <> NULL",
                    &st, &err),
              Err::kOk)
        << err;
    ASSERT_TRUE(st.select.where.has_value());
    const Condition& root = *st.select.where;
    ASSERT_EQ(root.kind, Condition::Kind::kAnd);
    ASSERT_EQ(root.children.size(), 7u);
    EXPECT_EQ(root.children[0].op, CompareOp::kEq);
    EXPECT_EQ(root.children[1].op, CompareOp::kNe);
    EXPECT_DOUBLE_EQ(root.children[1].value.as_real(), 2.5);
    EXPECT_EQ(root.children[2].op, CompareOp::kLt);
    EXPECT_EQ(root.children[2].value.as_bytes(), "x");
    EXPECT_EQ(root.children[3].op, CompareOp::kLe);
    EXPECT_TRUE(root.children[3].value.type() == rtdb::DataType::kBool);
    EXPECT_TRUE(root.children[3].value.as_bool());
    EXPECT_EQ(root.children[4].op, CompareOp::kGt);
    EXPECT_EQ(root.children[4].value.as_int(), -3);
    EXPECT_EQ(root.children[5].op, CompareOp::kGe);
    EXPECT_EQ(root.children[6].op, CompareOp::kNe);
    EXPECT_TRUE(root.children[6].value.is_null());
}

TEST(SqlParserTest, SelectOrderByLimitOffset) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("SELECT id FROM t ORDER BY a ASC, b DESC LIMIT 10 OFFSET 5", &st, &err),
              Err::kOk)
        << err;
    ASSERT_EQ(st.select.order_by.size(), 2u);
    EXPECT_EQ(st.select.order_by[0].column, "a");
    EXPECT_FALSE(st.select.order_by[0].descending);
    EXPECT_EQ(st.select.order_by[1].column, "b");
    EXPECT_TRUE(st.select.order_by[1].descending);
    EXPECT_EQ(st.select.limit, 10);
    EXPECT_EQ(st.select.offset, 5);
}

TEST(SqlParserTest, SelectIsNotNull) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("SELECT * FROM t WHERE a IS NOT NULL", &st, &err), Err::kOk) << err;
    ASSERT_TRUE(st.select.where.has_value());
    EXPECT_EQ(st.select.where->kind, Condition::Kind::kIsNotNull);
    EXPECT_EQ(st.select.where->column, "a");
}

TEST(SqlParserTest, InsertMultiRowWithLiterals) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("INSERT INTO t (id, name, score) VALUES (1, 'a''b', 1.5), (2, 'x', NULL)",
                    &st, &err),
              Err::kOk)
        << err;
    EXPECT_EQ(st.type, StatementType::kInsert);
    EXPECT_EQ(st.insert.table, "t");
    ASSERT_EQ(st.insert.columns.size(), 3u);
    ASSERT_EQ(st.insert.rows.size(), 2u);
    ASSERT_EQ(st.insert.rows[0].size(), 3u);
    EXPECT_EQ(st.insert.rows[0][0].as_int(), 1);
    EXPECT_EQ(st.insert.rows[0][1].as_bytes(), "a'b");
    EXPECT_DOUBLE_EQ(st.insert.rows[0][2].as_real(), 1.5);
    EXPECT_TRUE(st.insert.rows[1][2].is_null());
}

TEST(SqlParserTest, InsertColumnValueCountMismatch) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("INSERT INTO t (a, b) VALUES (1)", &st, &err), Err::kSqlSyntax);
    EXPECT_FALSE(err.empty());
}

TEST(SqlParserTest, InsertDuplicateColumnRejected) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("INSERT INTO t (a, a) VALUES (1, 2)", &st, &err), Err::kSqlSyntax);
}

TEST(SqlParserTest, UpdateWithWhere) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("UPDATE t SET a = 1, b = 'x' WHERE id = 7", &st, &err), Err::kOk) << err;
    EXPECT_EQ(st.type, StatementType::kUpdate);
    ASSERT_EQ(st.update.assignments.size(), 2u);
    EXPECT_EQ(st.update.assignments[0].column, "a");
    EXPECT_EQ(st.update.assignments[0].value.as_int(), 1);
    EXPECT_EQ(st.update.assignments[1].value.as_bytes(), "x");
    ASSERT_TRUE(st.update.where.has_value());
    EXPECT_EQ(st.update.where->column, "id");
}

TEST(SqlParserTest, UpdateDuplicateAssignmentRejected) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("UPDATE t SET a = 1, a = 2", &st, &err), Err::kSqlSyntax);
}

TEST(SqlParserTest, DeleteWithAndWithoutWhere) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("DELETE FROM t", &st, &err), Err::kOk) << err;
    EXPECT_EQ(st.type, StatementType::kDelete);
    EXPECT_FALSE(st.del.where.has_value());
    ASSERT_EQ(Parse("DELETE FROM t WHERE id >= 3", &st, &err), Err::kOk) << err;
    ASSERT_TRUE(st.del.where.has_value());
    EXPECT_EQ(st.del.where->op, CompareOp::kGe);
}

TEST(SqlParserTest, SemicolonAndLineCommentAccepted) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("-- leading comment\nSELECT id FROM t;  -- trailing", &st, &err), Err::kOk)
        << err;
    EXPECT_EQ(st.select.table, "t");
}

TEST(SqlParserTest, EmptyAndWhitespaceRejected) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("", &st, &err), Err::kSqlSyntax);
    EXPECT_EQ(Parse("   \n\t-- only comment\n", &st, &err), Err::kSqlSyntax);
}

TEST(SqlParserTest, UnterminatedString) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("SELECT * FROM t WHERE s = 'abc", &st, &err), Err::kSqlSyntax);
    EXPECT_NE(err.find("string"), std::string::npos) << err;
}

TEST(SqlParserTest, TrailingTokensRejected) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("SELECT * FROM t extra", &st, &err), Err::kSqlSyntax);
}

TEST(SqlParserTest, LimitMustBeNonNegativeInteger) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("SELECT * FROM t LIMIT -1", &st, &err), Err::kSqlSyntax);
}

TEST(SqlParserTest, UnsupportedJoinReturnsUnsupported) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("SELECT * FROM a JOIN b ON a.id = b.id", &st, &err), Err::kUnsupported);
    EXPECT_FALSE(err.empty());
}

TEST(SqlParserTest, UnsupportedAggregateAndGroupBy) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("SELECT count(*) FROM t", &st, &err), Err::kUnsupported);
    EXPECT_EQ(Parse("SELECT * FROM t GROUP BY id", &st, &err), Err::kUnsupported);
    EXPECT_EQ(Parse("SELECT * FROM t HAVING a = 1", &st, &err), Err::kUnsupported);
    EXPECT_EQ(Parse("SELECT DISTINCT id FROM t", &st, &err), Err::kUnsupported);
}

TEST(SqlParserTest, UnsupportedFunctionInWhere) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("SELECT * FROM t WHERE f(x) = 1", &st, &err), Err::kUnsupported);
}

TEST(SqlParserTest, UnsupportedSubqueryReturnsUnsupported) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("SELECT * FROM t WHERE id IN (SELECT id FROM u)", &st, &err),
              Err::kUnsupported);
}

TEST(SqlParserTest, UnsupportedMultipleStatements) {
    Statement st;
    std::string err;
    EXPECT_EQ(Parse("SELECT * FROM t; DELETE FROM t", &st, &err), Err::kUnsupported);
}

TEST(SqlParserTest, NullLiteralsInInsert) {
    Statement st;
    std::string err;
    ASSERT_EQ(Parse("INSERT INTO t (a, b) VALUES (NULL, FALSE)", &st, &err), Err::kOk) << err;
    ASSERT_EQ(st.insert.rows.size(), 1u);
    EXPECT_TRUE(st.insert.rows[0][0].is_null());
    EXPECT_FALSE(st.insert.rows[0][1].as_bool());
}

TEST(SqlParserTest, NamesAndOperatorsDiagnostics) {
    EXPECT_STREQ(rtdb::sql::StatementTypeName(StatementType::kSelect), "SELECT");
    EXPECT_STREQ(rtdb::sql::StatementTypeName(StatementType::kDelete), "DELETE");
    EXPECT_STREQ(rtdb::sql::CompareOpName(CompareOp::kNe), "<>");
    EXPECT_STREQ(rtdb::sql::CompareOpName(CompareOp::kGe), ">=");
}

}  // namespace
