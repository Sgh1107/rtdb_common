/// \file
/// \brief M4-W3 内嵌 SQL 执行器（docs/16 D5/D6/D7）。
///
/// 设计要点：
///  - 单语句原子：多行 INSERT / UPDATE / DELETE 一律走一次
///    `Engine::ExecuteBatch`（WAL 三段式 + 批前校验，失败零副作用）；
///    不提供跨语句事务（M5）。
///  - SELECT 分页：优先把 WHERE 中对主键列的比较下推成 `KeyRange`，
///    再以 `ScanLimited` 按页读取（上一页末行主键开区间续扫），避免
///    一次性物化全表；内存侧做 WHERE 过滤 / ORDER BY / LIMIT / 投影。
///  - 预算：`ExecuteOptions::max_rows` 同时约束 SELECT 物化行数与
///    UPDATE/DELETE 的目标行数，超限返回 `kLimitExceeded`。
///  - 零第三方依赖、零异常穿越；错误一律 Err + 可选诊断字符串。

#include "rtdb/sql.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rtdb/column_value.hpp"
#include "rtdb/engine.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"
#include "rtdb/table_def.hpp"
#include "rtdb/table_handle.hpp"

namespace rtdb {
namespace sql {
namespace {

/// RTDB_SQL_MAX_ROWS 默认预算（docs/16 D5）。
constexpr uint64_t kDefaultMaxRows = 100000;
/// SELECT 单次 ScanLimited 的页大小（主键有序；避免一次物化整表）。
constexpr size_t kScanPageRows = 512;

uint64_t DefaultMaxRowsImpl() noexcept { return kDefaultMaxRows; }

bool IsIntegerType(DataType t) noexcept {
    return t == DataType::kInt8 || t == DataType::kInt16 || t == DataType::kInt32 ||
           t == DataType::kInt64 || t == DataType::kBool || t == DataType::kUnixTsNs;
}
bool IsRealType(DataType t) noexcept {
    return t == DataType::kFloat32 || t == DataType::kFloat64;
}
bool IsVarType(DataType t) noexcept {
    return t == DataType::kUtf8Var || t == DataType::kBytesVar;
}

Err FailWith(std::string* err, Err code, std::string msg) {
    if (err != nullptr) *err = std::move(msg);
    return code;
}

bool ParseIntText(const std::string& s, int64_t* out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0') return false;
    *out = static_cast<int64_t>(v);
    return true;
}

bool ParseRealText(const std::string& s, double* out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0') return false;
    *out = v;
    return true;
}

/// \brief 把 SQL 字面量转成与列类型自洽的 ColumnValue。
///
/// 列值写入路径（TableStore::BuildRecord）按 `ValueTypeCompatible`
/// 校验值类型标注，故整数列须产整数类、浮点列产 kFloat64、变长列产
/// 变长类；数值字符串允许按十进制解析（友好面），类型不兼容明确报错。
Err CoerceLiteral(const ColumnValue& in, const ColumnDef& col, ColumnValue* out,
                  std::string* err) {
    if (in.is_null()) {
        if (!col.nullable)
            return FailWith(err, Err::kInvalidArgument,
                            "column '" + col.name + "' is NOT NULL");
        *out = ColumnValue::Null();
        return Err::kOk;
    }
    if (IsIntegerType(col.type)) {
        int64_t v = 0;
        switch (in.type()) {
            case DataType::kInt8:
            case DataType::kInt16:
            case DataType::kInt32:
            case DataType::kInt64:
            case DataType::kBool:
            case DataType::kUnixTsNs:
                v = in.as_int();
                break;
            case DataType::kFloat32:
            case DataType::kFloat64:
                v = static_cast<int64_t>(in.as_real());
                break;
            case DataType::kUtf8Var:
            case DataType::kBytesVar:
                if (!ParseIntText(in.as_bytes(), &v))
                    return FailWith(err, Err::kInvalidArgument,
                                    "value is not an integer for column '" + col.name + "'");
                break;
            default:
                return FailWith(err, Err::kInvalidArgument,
                                "incompatible value for column '" + col.name + "'");
        }
        switch (col.type) {
            case DataType::kInt8:
                if (v < -128 || v > 127)
                    return FailWith(err, Err::kInvalidArgument,
                                    "int8 overflow for column '" + col.name + "'");
                break;
            case DataType::kInt16:
                if (v < -32768 || v > 32767)
                    return FailWith(err, Err::kInvalidArgument,
                                    "int16 overflow for column '" + col.name + "'");
                break;
            case DataType::kInt32:
                if (v < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
                    v > static_cast<int64_t>(std::numeric_limits<int32_t>::max()))
                    return FailWith(err, Err::kInvalidArgument,
                                    "int32 overflow for column '" + col.name + "'");
                break;
            case DataType::kBool:
                if (v != 0 && v != 1)
                    return FailWith(err, Err::kInvalidArgument,
                                    "bool expects 0 or 1 for column '" + col.name + "'");
                break;
            default:
                break;
        }
        *out = ColumnValue::IntOf(col.type, v);
        return Err::kOk;
    }
    if (IsRealType(col.type)) {
        double d = 0.0;
        switch (in.type()) {
            case DataType::kInt8:
            case DataType::kInt16:
            case DataType::kInt32:
            case DataType::kInt64:
            case DataType::kBool:
            case DataType::kUnixTsNs:
                d = static_cast<double>(in.as_int());
                break;
            case DataType::kFloat32:
            case DataType::kFloat64:
                d = in.as_real();
                break;
            case DataType::kUtf8Var:
            case DataType::kBytesVar:
                if (!ParseRealText(in.as_bytes(), &d))
                    return FailWith(err, Err::kInvalidArgument,
                                    "value is not numeric for column '" + col.name + "'");
                break;
            default:
                return FailWith(err, Err::kInvalidArgument,
                                "incompatible value for column '" + col.name + "'");
        }
        *out = ColumnValue::Real(d);
        return Err::kOk;
    }
    if (IsVarType(col.type)) {
        if (!IsVarType(in.type()))
            return FailWith(err, Err::kInvalidArgument,
                            "column '" + col.name + "' expects a string value");
        *out = col.type == DataType::kBytesVar ? ColumnValue::Bytes(in.as_bytes())
                                               : ColumnValue::Text(in.as_bytes());
        return Err::kOk;
    }
    return FailWith(err, Err::kUnsupported,
                    "unsupported column type for '" + col.name + "'");
}

/// 同类型两值比较：-1 / 0 / 1。NULL 约定最小（ORDER BY ASC 时排在最前）。
int CompareCells(const ColumnValue& a, const ColumnValue& b, DataType t) noexcept {
    if (a.is_null() || b.is_null()) {
        if (a.is_null() && b.is_null()) return 0;
        return a.is_null() ? -1 : 1;
    }
    if (IsIntegerType(t)) {
        const int64_t x = a.as_int();
        const int64_t y = b.as_int();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (IsRealType(t)) {
        const double x = a.as_real();
        const double y = b.as_real();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    const std::string& x = a.as_bytes();
    const std::string& y = b.as_bytes();
    const int c = x.compare(y);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

/// 表 schema 快照 + 列名索引（标识符大小写敏感，D4）。
struct Schema {
    std::vector<ColumnDef> cols;
    std::unordered_map<std::string, size_t> by_name;

    Err Build(const std::vector<ColumnDef>& defs, std::string* err) {
        cols = defs;
        by_name.clear();
        if (cols.empty()) return FailWith(err, Err::kTableNotFound, "table has no columns");
        for (size_t i = 0; i < cols.size(); ++i) {
            if (!by_name.emplace(cols[i].name, i).second)
                return FailWith(err, Err::kInvalidArgument,
                                "duplicate column '" + cols[i].name + "' in schema");
        }
        return Err::kOk;
    }

    Err IndexOf(const std::string& name, size_t* out, std::string* err) const {
        const auto it = by_name.find(name);
        if (it == by_name.end())
            return FailWith(err, Err::kInvalidArgument, "unknown column '" + name + "'");
        *out = it->second;
        return Err::kOk;
    }
};

using RowValues = std::vector<ColumnValue>;

/// WHERE 求值：SQL 三值逻辑按"UNKNOWN 不入选"处理（NULL 与任何值比较
/// 结果均为 false；IS [NOT] NULL 单独判定）。
Err EvalCondition(const Condition& c, const Schema& schema, const RowValues& row,
                  bool* out, std::string* err) {
    switch (c.kind) {
        case Condition::Kind::kAnd:
        case Condition::Kind::kOr: {
            const bool is_and = c.kind == Condition::Kind::kAnd;
            bool acc = is_and;
            for (const Condition& child : c.children) {
                bool v = false;
                const Err e = EvalCondition(child, schema, row, &v, err);
                if (!IsOk(e)) return e;
                // 不短路：所有子条件都参与校验，未知列/类型错误不会被
                // 前面的结果掩盖（与"零副作用失败"的执行契约一致）。
                acc = is_and ? (acc && v) : (acc || v);
            }
            *out = acc;
            return Err::kOk;
        }
        case Condition::Kind::kNot: {
            if (c.children.size() != 1)
                return FailWith(err, Err::kInternal, "malformed NOT expression");
            bool v = false;
            const Err e = EvalCondition(c.children[0], schema, row, &v, err);
            if (!IsOk(e)) return e;
            *out = !v;
            return Err::kOk;
        }
        case Condition::Kind::kIsNull:
        case Condition::Kind::kIsNotNull: {
            size_t idx = 0;
            const Err ie = schema.IndexOf(c.column, &idx, err);
            if (!IsOk(ie)) return ie;
            const bool is_null = row[idx].is_null();
            *out = c.kind == Condition::Kind::kIsNull ? is_null : !is_null;
            return Err::kOk;
        }
        case Condition::Kind::kCompare:
        default:
            break;
    }
    size_t idx = 0;
    const Err ie = schema.IndexOf(c.column, &idx, err);
    if (!IsOk(ie)) return ie;
    // `col = NULL` / `col <> NULL` 恒为 UNKNOWN（不报错、不入选）。
    if (c.value.is_null() || row[idx].is_null()) {
        *out = false;
        return Err::kOk;
    }
    ColumnValue lit;
    const Err ce = CoerceLiteral(c.value, schema.cols[idx], &lit, err);
    if (!IsOk(ce)) return ce;
    const int cmp = CompareCells(row[idx], lit, schema.cols[idx].type);
    switch (c.op) {
        case CompareOp::kEq: *out = cmp == 0; break;
        case CompareOp::kNe: *out = cmp != 0; break;
        case CompareOp::kLt: *out = cmp < 0; break;
        case CompareOp::kLe: *out = cmp <= 0; break;
        case CompareOp::kGt: *out = cmp > 0; break;
        case CompareOp::kGe: *out = cmp >= 0; break;
        default: return FailWith(err, Err::kInternal, "unknown compare operator");
    }
    return Err::kOk;
}

/// 主键范围下推：仅沿 AND 树提取对列 0 的比较，其它条件保留内存过滤。
struct KeyBounds {
    ColumnValue lo = ColumnValue::Null();  ///< Null = 无界
    ColumnValue hi = ColumnValue::Null();
    bool lo_inclusive = true;
    bool hi_inclusive = true;
};

void MergeBound(bool is_low, const ColumnValue& value, bool inclusive, DataType key_type,
                KeyBounds* out) {
    ColumnValue& cur = is_low ? out->lo : out->hi;
    bool& cur_incl = is_low ? out->lo_inclusive : out->hi_inclusive;
    if (cur.is_null()) {
        cur = value;
        cur_incl = inclusive;
        return;
    }
    const int cmp = CompareCells(value, cur, key_type);
    const bool stricter = is_low ? cmp > 0 : cmp < 0;
    if (stricter) {
        cur = value;
        cur_incl = inclusive;
    } else if (cmp == 0 && !inclusive) {
        cur_incl = false;  // 同值边界：开区间严于闭区间
    }
}

void ExtractKeyBounds(const Condition* c, const Schema& schema, KeyBounds* out) {
    if (c == nullptr || schema.cols.empty()) return;
    if (c->kind == Condition::Kind::kAnd) {
        for (const Condition& child : c->children)
            ExtractKeyBounds(&child, schema, out);
        return;
    }
    if (c->kind != Condition::Kind::kCompare || c->column != schema.cols[0].name) return;
    if (c->value.is_null() || c->op == CompareOp::kNe) return;
    ColumnValue v;
    std::string ignored;
    if (!IsOk(CoerceLiteral(c->value, schema.cols[0], &v, &ignored))) return;
    const DataType kt = schema.cols[0].type;
    switch (c->op) {
        case CompareOp::kEq:
            MergeBound(true, v, true, kt, out);
            MergeBound(false, v, true, kt, out);
            break;
        case CompareOp::kGt:
        case CompareOp::kGe:
            MergeBound(true, v, c->op == CompareOp::kGe, kt, out);
            break;
        case CompareOp::kLt:
        case CompareOp::kLe:
            MergeBound(false, v, c->op == CompareOp::kLe, kt, out);
            break;
        default:
            break;
    }
}

/// 分页扫描 + 内存 WHERE 过滤。
///
/// \param max_rows    物化行数预算（超过返回 kLimitExceeded）
/// \param stop_after  0 = 不限；否则达到该行数即正常停止（SELECT 无
///                    WHERE/ORDER BY 且带 LIMIT 时的提前收口）
Err ScanRows(Engine& eng, const TableHandle& table, const Schema& schema,
             const Condition* where, uint64_t max_rows, uint64_t stop_after,
             std::vector<RowValues>* out, std::string* err) {
    KeyBounds kb;
    ExtractKeyBounds(where, schema, &kb);
    Engine::KeyRange base;
    base.lo = kb.lo;
    base.hi = kb.hi;
    base.lo_inclusive = kb.lo_inclusive;
    base.hi_inclusive = kb.hi_inclusive;

    ColumnValue cursor = kb.lo;
    bool first_page = true;
    for (;;) {
        Engine::KeyRange page = base;
        page.lo = cursor;
        page.lo_inclusive = first_page ? kb.lo_inclusive : false;
        bool more = false;
        auto scanned = eng.ScanLimited(table, page, kScanPageRows, &more);
        if (!scanned.IsOk()) return scanned.Error();
        std::vector<RowValues> rows = scanned.TakeValue();
        if (rows.empty()) break;
        // 续扫游标必须在行被 move 之前取（页内主键升序）。
        const ColumnValue last_key = rows.back()[0];
        for (RowValues& r : rows) {
            bool matched = true;
            if (where != nullptr) {
                const Err e = EvalCondition(*where, schema, r, &matched, err);
                if (!IsOk(e)) return e;
            }
            if (!matched) continue;
            if (out->size() >= max_rows)
                return FailWith(err, Err::kLimitExceeded,
                                "SQL row budget exceeded (max_rows=" +
                                    std::to_string(max_rows) + ")");
            out->push_back(std::move(r));
            if (stop_after != 0 && out->size() >= stop_after) return Err::kOk;
        }
        cursor = last_key;
        first_page = false;
        if (!more) break;
    }
    return Err::kOk;
}

/// 排序（在投影之前，允许 ORDER BY 引用未投影列）。
Err SortRows(const std::vector<OrderBy>& order_by, const Schema& schema,
             std::vector<RowValues>* rows, std::string* err) {
    if (order_by.empty()) return Err::kOk;
    struct Key {
        size_t index;
        DataType type;
        bool descending;
    };
    std::vector<Key> keys;
    keys.reserve(order_by.size());
    for (const OrderBy& ob : order_by) {
        size_t idx = 0;
        const Err e = schema.IndexOf(ob.column, &idx, err);
        if (!IsOk(e)) return e;
        keys.push_back(Key{idx, schema.cols[idx].type, ob.descending});
    }
    std::stable_sort(rows->begin(), rows->end(),
                     [&keys](const RowValues& a, const RowValues& b) {
                         for (const Key& k : keys) {
                             const int cmp = CompareCells(a[k.index], b[k.index], k.type);
                             if (cmp != 0) return k.descending ? cmp > 0 : cmp < 0;
                         }
                         return false;
                     });
    return Err::kOk;
}

Err ExecSelect(Engine& eng, const Statement& st, const TableHandle& table,
               const Schema& schema, uint64_t max_rows, SqlResult* out, std::string* err) {
    const SelectStatement& q = st.select;
    const Condition* where = q.where.has_value() ? &*q.where : nullptr;
    uint64_t stop_after = 0;
    if (where == nullptr && q.order_by.empty() && q.limit >= 0) {
        const uint64_t lim = static_cast<uint64_t>(q.limit);
        const uint64_t off = q.offset < 0 ? 0 : static_cast<uint64_t>(q.offset);
        uint64_t want = max_rows;
        if (lim < max_rows && off < max_rows - lim) want = lim + off;
        stop_after = want;
    }
    std::vector<RowValues> matched;
    const Err se = ScanRows(eng, table, schema, where, max_rows, stop_after, &matched, err);
    if (!IsOk(se)) return se;
    const Err oe = SortRows(q.order_by, schema, &matched, err);
    if (!IsOk(oe)) return oe;

    // 投影列：SELECT * = 全列；显式列按名取索引（别名不进入结果列名，D4）。
    std::vector<size_t> projection;
    if (q.select_all) {
        projection.reserve(schema.cols.size());
        for (size_t i = 0; i < schema.cols.size(); ++i) projection.push_back(i);
    } else {
        projection.reserve(q.columns.size());
        for (const std::string& name : q.columns) {
            size_t idx = 0;
            const Err e = schema.IndexOf(name, &idx, err);
            if (!IsOk(e)) return e;
            projection.push_back(idx);
        }
    }
    for (size_t idx : projection)
        out->columns.push_back(SqlColumnMeta{schema.cols[idx].name, schema.cols[idx].type});

    const uint64_t offset = q.offset < 0 ? 0 : static_cast<uint64_t>(q.offset);
    const uint64_t begin = offset < matched.size() ? offset : matched.size();
    uint64_t end = matched.size();
    if (q.limit >= 0) {
        const uint64_t lim = static_cast<uint64_t>(q.limit);
        if (lim < end - begin) end = begin + lim;
    }
    out->rows.reserve(static_cast<size_t>(end - begin));
    for (uint64_t i = begin; i < end; ++i) {
        RowValues projected;
        projected.reserve(projection.size());
        for (size_t idx : projection) projected.push_back(matched[static_cast<size_t>(i)][idx]);
        out->rows.push_back(std::move(projected));
    }
    out->affected_rows = 0;
    return Err::kOk;
}

/// 按列名映射构造完整行；未提供的列填 NULL（NOT NULL 列由 BuildRecord
/// 返回 kInvalidArgument，与 UpsertBatch 的"非空缺列"语义一致）。
Err BuildInsertRow(const InsertStatement& ins, const std::vector<ColumnValue>& literals,
                   const Schema& schema, RowValues* out, ColumnValue* primary_key,
                   std::string* err) {
    RowValues row(schema.cols.size(), ColumnValue::Null());
    std::vector<bool> provided(schema.cols.size(), false);
    for (size_t i = 0; i < ins.columns.size(); ++i) {
        size_t idx = 0;
        const Err e = schema.IndexOf(ins.columns[i], &idx, err);
        if (!IsOk(e)) return e;
        if (provided[idx])
            return FailWith(err, Err::kInvalidArgument,
                            "duplicate column '" + ins.columns[i] + "' in INSERT");
        const Err ce = CoerceLiteral(literals[i], schema.cols[idx], &row[idx], err);
        if (!IsOk(ce)) return ce;
        provided[idx] = true;
    }
    if (!provided[0] || row[0].is_null())
        return FailWith(err, Err::kInvalidArgument,
                        "INSERT must provide a non-NULL primary key '" +
                            schema.cols[0].name + "'");
    *primary_key = row[0];
    *out = std::move(row);
    return Err::kOk;
}

Err CommitBatch(Engine& eng, const std::vector<Engine::BatchOp>& ops, std::string* err) {
    if (ops.empty()) return Err::kOk;
    auto res = eng.ExecuteBatch(ops);
    if (!res.IsOk()) return FailWith(err, res.Error(), "batch failed: " +
                                                           std::string(ErrorMessage(res.Error())));
    return Err::kOk;
}

Err ExecInsert(Engine& eng, const Statement& st, const TableHandle& table,
               const Schema& schema, uint64_t max_rows, SqlResult* out, std::string* err) {
    const InsertStatement& ins = st.insert;
    if (ins.rows.empty()) return FailWith(err, Err::kInvalidArgument, "empty VALUES");
    if (ins.rows.size() > max_rows)
        return FailWith(err, Err::kLimitExceeded, "INSERT exceeds SQL row budget");
    for (const auto& r : ins.rows) {
        if (r.size() != ins.columns.size())
            return FailWith(err, Err::kInvalidArgument, "VALUES column count mismatch");
    }
    std::vector<Engine::BatchOp> ops;
    ops.reserve(ins.rows.size());
    std::set<int64_t> seen_keys;
    for (const auto& literals : ins.rows) {
        RowValues row;
        ColumnValue pk;
        const Err e = BuildInsertRow(ins, literals, schema, &row, &pk, err);
        if (!IsOk(e)) return e;
        if (!seen_keys.insert(pk.as_int()).second)
            return FailWith(err, Err::kAlreadyExists,
                            "duplicate primary key inside INSERT batch");
        Engine::BatchOp op;
        op.type = Engine::BatchOp::Type::kInsert;
        op.table = table;
        op.row = std::move(row);
        ops.push_back(std::move(op));
    }
    const Err be = CommitBatch(eng, ops, err);
    if (!IsOk(be)) return be;
    out->affected_rows = ins.rows.size();
    return Err::kOk;
}

/// UPDATE/DELETE 共用的目标行收集（按 WHERE 过滤，受 max_rows 预算约束）。
Err CollectTargets(Engine& eng, const TableHandle& table, const Schema& schema,
                   const Condition* where, uint64_t max_rows,
                   std::vector<RowValues>* out, std::string* err) {
    return ScanRows(eng, table, schema, where, max_rows, 0, out, err);
}

Err ExecUpdate(Engine& eng, const Statement& st, const TableHandle& table,
               const Schema& schema, uint64_t max_rows, SqlResult* out, std::string* err) {
    const UpdateStatement& up = st.update;
    // 赋值项预解析（列名 + 目标类型值），主键不可变更。
    std::vector<std::pair<size_t, ColumnValue>> assigns;
    assigns.reserve(up.assignments.size());
    for (const Assignment& a : up.assignments) {
        size_t idx = 0;
        const Err ie = schema.IndexOf(a.column, &idx, err);
        if (!IsOk(ie)) return ie;
        if (idx == 0)
            return FailWith(err, Err::kInvalidArgument,
                            "primary key '" + a.column + "' cannot be updated");
        ColumnValue v;
        const Err ce = CoerceLiteral(a.value, schema.cols[idx], &v, err);
        if (!IsOk(ce)) return ce;
        assigns.emplace_back(idx, std::move(v));
    }
    const Condition* where = up.where.has_value() ? &*up.where : nullptr;
    std::vector<RowValues> targets;
    const Err se = CollectTargets(eng, table, schema, where, max_rows, &targets, err);
    if (!IsOk(se)) return se;
    std::vector<Engine::BatchOp> ops;
    ops.reserve(targets.size());
    for (RowValues& row : targets) {
        for (const auto& a : assigns) row[a.first] = a.second;
        Engine::BatchOp op;
        op.type = Engine::BatchOp::Type::kUpdate;
        op.table = table;
        op.row = std::move(row);
        ops.push_back(std::move(op));
    }
    const Err be = CommitBatch(eng, ops, err);
    if (!IsOk(be)) return be;
    out->affected_rows = targets.size();
    return Err::kOk;
}

Err ExecDelete(Engine& eng, const Statement& st, const TableHandle& table,
               const Schema& schema, uint64_t max_rows, SqlResult* out, std::string* err) {
    const Condition* where = st.del.where.has_value() ? &*st.del.where : nullptr;
    std::vector<RowValues> targets;
    const Err se = CollectTargets(eng, table, schema, where, max_rows, &targets, err);
    if (!IsOk(se)) return se;
    std::vector<Engine::BatchOp> ops;
    ops.reserve(targets.size());
    for (const RowValues& row : targets) {
        Engine::BatchOp op;
        op.type = Engine::BatchOp::Type::kDelete;
        op.table = table;
        op.row.push_back(row[0]);  // delete = 仅主键（ExecuteBatch 契约）
        ops.push_back(std::move(op));
    }
    const Err be = CommitBatch(eng, ops, err);
    if (!IsOk(be)) return be;
    out->affected_rows = targets.size();
    return Err::kOk;
}

std::string TableNameOf(const Statement& st) {
    switch (st.type) {
        case StatementType::kSelect: return st.select.table;
        case StatementType::kInsert: return st.insert.table;
        case StatementType::kUpdate: return st.update.table;
        case StatementType::kDelete: return st.del.table;
    }
    return std::string();
}

}  // namespace

uint64_t DefaultMaxRows() noexcept { return DefaultMaxRowsImpl(); }

Result<SqlResult> Execute(Engine& eng, std::string_view sql, const ExecuteOptions& options,
                          std::string* error) {
    if (error != nullptr) error->clear();
    Statement st;
    const Err pe = Parse(sql, &st, error);
    if (!IsOk(pe)) return Result<SqlResult>::Fail(pe);

    auto table_res = eng.OpenTable(TableNameOf(st));
    if (!table_res.IsOk()) return Result<SqlResult>::Fail(table_res.Error());
    const TableHandle table = table_res.TakeValue();

    auto defs_res = eng.DescribeTable(table);
    if (!defs_res.IsOk()) return Result<SqlResult>::Fail(defs_res.Error());
    Schema schema;
    const Err be = schema.Build(defs_res.Value(), error);
    if (!IsOk(be)) return Result<SqlResult>::Fail(be);

    const uint64_t max_rows = options.max_rows == 0 ? kDefaultMaxRows : options.max_rows;
    SqlResult result;
    result.statement = st.type;
    Err e = Err::kOk;
    switch (st.type) {
        case StatementType::kSelect:
            e = ExecSelect(eng, st, table, schema, max_rows, &result, error);
            break;
        case StatementType::kInsert:
            e = ExecInsert(eng, st, table, schema, max_rows, &result, error);
            break;
        case StatementType::kUpdate:
            e = ExecUpdate(eng, st, table, schema, max_rows, &result, error);
            break;
        case StatementType::kDelete:
            e = ExecDelete(eng, st, table, schema, max_rows, &result, error);
            break;
        default:
            e = FailWith(error, Err::kUnsupported, "unknown statement type");
            break;
    }
    if (!IsOk(e)) return Result<SqlResult>::Fail(e);
    result.last_seq = eng.CommittedSeq();
    return Result<SqlResult>::Ok(std::move(result));
}

}  // namespace sql

// ---- Engine 内嵌 SQL API（docs/16 D6；实现落在 SQL 层，engine.cpp 不感知）----
Result<sql::SqlResult> Engine::ExecuteSql(std::string_view sql, std::string* error) {
    return sql::Execute(*this, sql, sql::ExecuteOptions{}, error);
}

Result<sql::SqlResult> Engine::ExecuteSql(std::string_view sql,
                                          const sql::ExecuteOptions& options,
                                          std::string* error) {
    return sql::Execute(*this, sql, options, error);
}

}  // namespace rtdb
