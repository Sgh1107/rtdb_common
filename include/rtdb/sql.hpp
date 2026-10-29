#ifndef RTDB_SQL_HPP_
#define RTDB_SQL_HPP_

/// \file
/// \brief M4 内嵌 SQL 单表子集：语法树 + 解析入口（docs/16 D4/D5）。
///
/// 语法子集（v1，冻结）：
///   SELECT <cols|*> FROM <table> [WHERE <expr>] [ORDER BY <col> [ASC|DESC][,...]]
///          [LIMIT n [OFFSET m]]
///   INSERT INTO <table> '(' <col>[,...] ')' VALUES '(' <literal>[,...] ')' [,...]
///   UPDATE <table> SET <col> = <literal>[, ...] [WHERE <expr>]
///   DELETE FROM <table> [WHERE <expr>]
///   expr := expr AND expr | expr OR expr | NOT expr | '(' expr ')'
///         | <col> <op> <literal> | <col> IS [NOT] NULL
///   op   := '=' | '<>' | '!=' | '<' | '<=' | '>' | '>='
///   literal := 整数 | 浮点 | 字符串（单引号，'' 转义） | TRUE/FALSE | NULL
///
/// 子集外语法（JOIN/子查询/聚合/GROUP BY/HAVING/UNION/函数/多语句等）由
/// `Parse` 明确返回 `kUnsupported`，不静默降级；词法/语法错误返回
/// `kSqlSyntax`（307）。解析器为零三方依赖的手写递归下降；本头文件不含
/// 执行语义（W3 执行器基于本 AST）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rtdb/api_macro.hpp"
#include "rtdb/column_value.hpp"
#include "rtdb/err.hpp"

namespace rtdb {
namespace sql {

/// 语句类型。
enum class StatementType { kSelect = 1, kInsert = 2, kUpdate = 3, kDelete = 4 };

/// 比较运算符（WHERE 字面量比较）。
enum class CompareOp {
    kEq = 1,
    kNe = 2,
    kLt = 3,
    kLe = 4,
    kGt = 5,
    kGe = 6,
};

/// 布尔表达式（WHERE 子句）。
struct Condition {
    enum class Kind {
        kCompare = 1,   ///< column op value
        kIsNull = 2,    ///< column IS NULL
        kIsNotNull = 3, ///< column IS NOT NULL
        kAnd = 4,       ///< children 连接（n≥2）
        kOr = 5,        ///< children 连接（n≥2）
        kNot = 6,       ///< children[0] 取反
    };
    Kind kind = Kind::kCompare;
    std::string column;              ///< kCompare/kIsNull/kIsNotNull
    CompareOp op = CompareOp::kEq;   ///< kCompare
    ColumnValue value;               ///< kCompare（可为 NULL）
    std::vector<Condition> children; ///< kAnd/kOr/kNot
};

/// ORDER BY 项。
struct OrderBy {
    std::string column;
    bool descending = false;
};

/// SELECT 语句。
struct SelectStatement {
    std::vector<std::string> columns; ///< 显式列（select_all=true 时为空）
    bool select_all = false;          ///< `SELECT *`
    std::string table;
    std::optional<Condition> where;
    std::vector<OrderBy> order_by;
    int64_t limit = -1;  ///< -1 = 无 LIMIT
    int64_t offset = 0;
};

/// INSERT 语句（多行 VALUES；每个 row 的列值数与 columns 一致）。
struct InsertStatement {
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::vector<ColumnValue>> rows;
};

/// UPDATE 赋值项。
struct Assignment {
    std::string column;
    ColumnValue value;
};

/// UPDATE 语句。
struct UpdateStatement {
    std::string table;
    std::vector<Assignment> assignments;
    std::optional<Condition> where;
};

/// DELETE 语句。
struct DeleteStatement {
    std::string table;
    std::optional<Condition> where;
};

/// 单条语句（仅 type 对应的成员有效）。
struct Statement {
    StatementType type = StatementType::kSelect;
    SelectStatement select;
    InsertStatement insert;
    UpdateStatement update;
    DeleteStatement del;
};

/// \brief 解析单条 SQL 语句。
///
/// \param sql   UTF-8 文本；允许结尾单个 `;` 与 `--` 行注释。
/// \param out   成功时填充 AST（非空）。
/// \param error 可选：失败时的简短说明（含字符偏移）。
/// \return kOk / kSqlSyntax（词法或语法错误）/ kUnsupported（子集外语法）/
///         kInvalidArgument（sql 或 out 为空）。
RTDB_API Err Parse(std::string_view sql, Statement* out, std::string* error = nullptr);

/// 语句类型名称（诊断/日志）。
RTDB_API const char* StatementTypeName(StatementType type) noexcept;

/// 比较运算符名称（诊断/日志）。
RTDB_API const char* CompareOpName(CompareOp op) noexcept;

}  // namespace sql
}  // namespace rtdb

#endif  // RTDB_SQL_HPP_
