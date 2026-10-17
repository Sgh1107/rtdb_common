#include "net/value_codec.hpp"

#include "rtdb/proto_v1.h"

namespace rtdb {
namespace net {

namespace {

/// DataType 线枚举取值范围（只增不改；见 table_def.hpp）。
constexpr uint8_t kDataTypeWireMin = 1;   // kInt8
constexpr uint8_t kDataTypeWireMax = 10;  // kUnixTsNs

bool IsValidWireType(uint8_t t) noexcept {
    return t >= kDataTypeWireMin && t <= kDataTypeWireMax;
}

bool IsValidName(std::string_view s) noexcept {
    return !s.empty() && s.size() <= RTDB_PROTO_MAX_NAME_BYTES;
}

}  // namespace

bool IsIntClass(DataType t) noexcept {
    return t == DataType::kInt8 || t == DataType::kInt16 || t == DataType::kInt32 ||
           t == DataType::kInt64 || t == DataType::kBool || t == DataType::kUnixTsNs;
}

bool IsFloatClass(DataType t) noexcept {
    return t == DataType::kFloat32 || t == DataType::kFloat64;
}

bool IsVarClass(DataType t) noexcept {
    return t == DataType::kUtf8Var || t == DataType::kBytesVar;
}

// ---- 值（行式）----

void WriteValue(Writer& w, const ColumnValue& v) noexcept {
    if (v.is_null()) {
        w.U8(RTDB_VAL_NULL);
        return;
    }
    if (IsFloatClass(v.type())) {
        w.U8(RTDB_VAL_F64);
        w.F64(v.as_real());
        return;
    }
    if (IsVarClass(v.type())) {
        w.U8(RTDB_VAL_VAR);
        w.Var(v.as_bytes());
        return;
    }
    w.U8(RTDB_VAL_I64);
    w.I64(v.as_int());
}

ColumnValue ReadValueTyped(Reader& r, DataType col_type) noexcept {
    const uint8_t tag = r.U8();
    if (!r.ok()) return ColumnValue::Null();
    if (tag == RTDB_VAL_NULL) return ColumnValue::Null();
    if (IsIntClass(col_type)) {
        if (tag != RTDB_VAL_I64) {
            r.Poison();
            return ColumnValue::Null();
        }
        return ColumnValue::IntOf(col_type, r.I64());
    }
    if (IsFloatClass(col_type)) {
        if (tag != RTDB_VAL_F64) {
            r.Poison();
            return ColumnValue::Null();
        }
        return ColumnValue::Real(r.F64());
    }
    if (IsVarClass(col_type)) {
        if (tag != RTDB_VAL_VAR) {
            r.Poison();
            return ColumnValue::Null();
        }
        std::string s;
        if (!r.Var(&s)) return ColumnValue::Null();
        return col_type == DataType::kUtf8Var ? ColumnValue::Text(std::move(s))
                                              : ColumnValue::Bytes(std::move(s));
    }
    r.Poison();  // 未覆盖的列类型（防御；调用方应以 schema 校验兜底）
    return ColumnValue::Null();
}

void WriteRow(Writer& w, const std::vector<ColumnValue>& row) noexcept {
    for (const ColumnValue& v : row) WriteValue(w, v);
}

ColumnValue ReadValueGeneric(Reader& r) noexcept {
    const uint8_t tag = r.U8();
    if (!r.ok()) return ColumnValue::Null();
    if (tag == RTDB_VAL_NULL) return ColumnValue::Null();
    if (tag == RTDB_VAL_I64) return ColumnValue::Int(r.I64());
    if (tag == RTDB_VAL_F64) return ColumnValue::Real(r.F64());
    if (tag == RTDB_VAL_VAR) {
        std::string s;
        if (!r.Var(&s)) return ColumnValue::Null();
        return ColumnValue::Text(std::move(s));
    }
    r.Poison();
    return ColumnValue::Null();
}

std::vector<ColumnValue> ReadRow(Reader& r, const std::vector<DataType>& col_types) noexcept {
    std::vector<ColumnValue> row;
    row.reserve(col_types.size());
    for (DataType t : col_types) {
        row.push_back(ReadValueTyped(r, t));
        if (!r.ok()) break;
    }
    return row;
}

// ---- 结果集/增量自描述头 ----

void WriteResultHeader(Writer& w, std::string_view table, Span<const ColumnDef> cols,
                       uint32_t n_rows) noexcept {
    w.Str(table);
    w.U32(static_cast<uint32_t>(cols.size()));
    for (const ColumnDef& c : cols) {
        w.Str(c.name);
        w.U8(static_cast<uint8_t>(c.type));
    }
    w.U32(n_rows);
}

Result<ResultSchema> ReadResultHeader(Reader& r) noexcept {
    ResultSchema s;
    if (!r.Str(&s.table) || !IsValidName(s.table)) {
        return Result<ResultSchema>::Fail(Err::kProtocolError);
    }
    const uint32_t n_cols = r.U32();
    if (!r.ok() || n_cols == 0 || n_cols > 4096) {
        return Result<ResultSchema>::Fail(Err::kProtocolError);
    }
    s.col_names.reserve(n_cols);
    s.col_types.reserve(n_cols);
    for (uint32_t i = 0; i < n_cols; ++i) {
        std::string name;
        if (!r.Str(&name) || !IsValidName(name)) {
            return Result<ResultSchema>::Fail(Err::kProtocolError);
        }
        const uint8_t t = r.U8();
        if (!r.ok() || !IsValidWireType(t)) {
            return Result<ResultSchema>::Fail(Err::kProtocolError);
        }
        s.col_names.push_back(std::move(name));
        s.col_types.push_back(static_cast<DataType>(t));
    }
    s.n_rows = r.U32();
    if (!r.ok()) return Result<ResultSchema>::Fail(Err::kProtocolError);
    return Result<ResultSchema>::Ok(std::move(s));
}

// ---- 表定义（DDL）----

namespace {

/// 默认值的线类型：浮点类列携带 real，其余携带 int。
bool DefaultIsReal(DataType t) noexcept { return IsFloatClass(t); }

}  // namespace

void WriteTableDef(Writer& w, const TableDef& def) noexcept {
    w.Str(def.name);
    w.U32(static_cast<uint32_t>(def.columns.size()));
    for (const ColumnDef& c : def.columns) {
        w.Str(c.name);
        w.U8(static_cast<uint8_t>(c.type));
        w.U32(c.len);
        w.U8(c.nullable ? 1 : 0);
        w.U8(c.has_default ? 1 : 0);
        if (c.has_default) {
            w.U8(c.default_value.is_null ? 1 : 0);
            if (!c.default_value.is_null) {
                const bool real = DefaultIsReal(c.type);
                w.U8(real ? 1 : 0);
                if (real) {
                    w.F64(c.default_value.real_value);
                } else {
                    w.I64(c.default_value.int_value);
                }
            }
        }
    }
    w.U32(static_cast<uint32_t>(def.primary_key.size()));
    for (const std::string_view pk : def.primary_key) w.Str(pk);
    w.U32(static_cast<uint32_t>(def.secondary.size()));
    for (const IndexDef& ix : def.secondary) {
        w.Str(ix.name);
        w.U8(ix.unique ? 1 : 0);
        w.U32(static_cast<uint32_t>(ix.columns.size()));
        for (const std::string_view cn : ix.columns) w.Str(cn);
    }
    w.U64(static_cast<uint64_t>(def.max_rows_hint));
    w.U32(def.row_cap);
}

Result<OwnedTableDef> ReadTableDef(Reader& r) noexcept {
    OwnedTableDef out;
    TableDef& d = out.def;

    if (!r.Str(&d.name) || !IsValidName(d.name)) {
        return Result<OwnedTableDef>::Fail(Err::kProtocolError);
    }
    const uint32_t n_cols = r.U32();
    if (!r.ok() || n_cols == 0 || n_cols > 4096) {
        return Result<OwnedTableDef>::Fail(Err::kProtocolError);
    }
    d.columns.resize(n_cols);
    for (uint32_t i = 0; i < n_cols; ++i) {
        ColumnDef& c = d.columns[i];
        if (!r.Str(&c.name) || !IsValidName(c.name)) {
            return Result<OwnedTableDef>::Fail(Err::kProtocolError);
        }
        const uint8_t t = r.U8();
        if (!r.ok() || !IsValidWireType(t)) {
            return Result<OwnedTableDef>::Fail(Err::kProtocolError);
        }
        c.type = static_cast<DataType>(t);
        c.len = r.U32();
        c.nullable = (r.U8() != 0);
        c.has_default = (r.U8() != 0);
        if (!r.ok()) return Result<OwnedTableDef>::Fail(Err::kProtocolError);
        if (c.has_default) {
            c.default_value.is_null = (r.U8() != 0);
            if (!r.ok()) return Result<OwnedTableDef>::Fail(Err::kProtocolError);
            if (!c.default_value.is_null) {
                const uint8_t kind = r.U8();
                if (!r.ok() || kind > 1) {
                    return Result<OwnedTableDef>::Fail(Err::kProtocolError);
                }
                if (kind == 1) {
                    c.default_value.real_value = r.F64();
                } else {
                    c.default_value.int_value = r.I64();
                }
            }
        }
    }

    const uint32_t n_pk = r.U32();
    if (!r.ok() || n_pk > 16) return Result<OwnedTableDef>::Fail(Err::kProtocolError);
    std::vector<std::string> pk_tmp;
    pk_tmp.reserve(n_pk);
    for (uint32_t i = 0; i < n_pk; ++i) {
        std::string s;
        if (!r.Str(&s) || !IsValidName(s)) {
            return Result<OwnedTableDef>::Fail(Err::kProtocolError);
        }
        pk_tmp.push_back(std::move(s));
    }

    const uint32_t n_sec = r.U32();
    if (!r.ok() || n_sec > 64) return Result<OwnedTableDef>::Fail(Err::kProtocolError);
    struct SecTmp {
        std::string name;
        bool unique = false;
        std::vector<std::string> cols;
    };
    std::vector<SecTmp> sec_tmp;
    sec_tmp.reserve(n_sec);
    for (uint32_t i = 0; i < n_sec; ++i) {
        SecTmp st;
        if (!r.Str(&st.name) || !IsValidName(st.name)) {
            return Result<OwnedTableDef>::Fail(Err::kProtocolError);
        }
        st.unique = (r.U8() != 0);
        const uint32_t n_ic = r.U32();
        if (!r.ok() || n_ic == 0 || n_ic > 16) {
            return Result<OwnedTableDef>::Fail(Err::kProtocolError);
        }
        st.cols.reserve(n_ic);
        for (uint32_t k = 0; k < n_ic; ++k) {
            std::string s;
            if (!r.Str(&s) || !IsValidName(s)) {
                return Result<OwnedTableDef>::Fail(Err::kProtocolError);
            }
            st.cols.push_back(std::move(s));
        }
        sec_tmp.push_back(std::move(st));
    }

    const uint64_t hint = r.U64();
    const uint32_t row_cap = r.U32();
    if (!r.ok()) return Result<OwnedTableDef>::Fail(Err::kProtocolError);
    d.max_rows_hint = static_cast<size_t>(hint);
    d.row_cap = row_cap;

    // 字符串入稳定存储（先定容、后回填视图，移动本对象不失效）。
    size_t total = pk_tmp.size();
    for (const SecTmp& st : sec_tmp) total += 1 + st.cols.size();
    out.storage.reserve(total);
    for (std::string& s : pk_tmp) out.storage.push_back(std::move(s));
    for (SecTmp& st : sec_tmp) {
        out.storage.push_back(std::move(st.name));
        for (std::string& c : st.cols) out.storage.push_back(std::move(c));
    }
    size_t idx = 0;
    d.primary_key.reserve(pk_tmp.size());
    for (size_t i = 0; i < pk_tmp.size(); ++i) {
        d.primary_key.push_back(std::string_view(out.storage[idx++]));
    }
    d.secondary.resize(sec_tmp.size());
    for (size_t i = 0; i < sec_tmp.size(); ++i) {
        IndexDef ix;
        ix.name = out.storage[idx++];
        ix.unique = sec_tmp[i].unique;
        ix.columns.reserve(sec_tmp[i].cols.size());
        for (size_t k = 0; k < sec_tmp[i].cols.size(); ++k) {
            ix.columns.push_back(std::string_view(out.storage[idx++]));
        }
        d.secondary[i] = std::move(ix);
    }
    return Result<OwnedTableDef>::Ok(std::move(out));
}

// ---- 列式块（EXEC_BATCH mode=1）----

void WriteBlock(Writer& w, const RecordBlockView& view) noexcept {
    const uint32_t n_rows = static_cast<uint32_t>(view.row_count);
    w.Str(view.table_name);
    w.U32(n_rows);
    w.U32(static_cast<uint32_t>(view.columns.size()));
    for (const BlockColumn& c : view.columns) {
        w.Str(c.name);
        uint8_t ptype = RTDB_BLKPT_I64;
        if (IsFloatClass(c.type)) {
            ptype = RTDB_BLKPT_F64;
        } else if (IsVarClass(c.type)) {
            ptype = RTDB_BLKPT_VAR;
        }
        w.U8(ptype);
        w.U8((c.null_flags != nullptr) ? 1 : 0);
        if (c.null_flags != nullptr) w.Raw(c.null_flags, n_rows);
        if (ptype == RTDB_BLKPT_I64) {
            const auto* v = static_cast<const int64_t*>(c.data);
            for (uint32_t i = 0; i < n_rows; ++i) w.I64(v[i]);
        } else if (ptype == RTDB_BLKPT_F64) {
            const auto* v = static_cast<const double*>(c.data);
            for (uint32_t i = 0; i < n_rows; ++i) w.F64(v[i]);
        } else {
            const auto* v = static_cast<const std::string_view*>(c.data);
            for (uint32_t i = 0; i < n_rows; ++i) w.Var(v[i]);
        }
    }
}

Result<OwnedBlock> ReadBlock(Reader& r) noexcept {
    OwnedBlock out;
    if (!r.Str(&out.table) || !IsValidName(out.table)) {
        return Result<OwnedBlock>::Fail(Err::kProtocolError);
    }
    const uint32_t n_rows = r.U32();
    const uint32_t n_cols = r.U32();
    if (!r.ok() || n_rows == 0 || n_rows > RTDB_PROTO_MAX_BLOCK_ROWS || n_cols == 0 ||
        n_cols > RTDB_PROTO_MAX_BLOCK_COLS) {
        return Result<OwnedBlock>::Fail(Err::kProtocolError);
    }
    out.row_count = n_rows;
    out.cols.reserve(n_cols);

    for (uint32_t i = 0; i < n_cols; ++i) {
        OwnedBlock::Col col;
        if (!r.Str(&col.name) || !IsValidName(col.name)) {
            return Result<OwnedBlock>::Fail(Err::kProtocolError);
        }
        const uint8_t ptype = r.U8();
        const uint8_t has_null = r.U8();
        if (!r.ok() || ptype > RTDB_BLKPT_VAR || has_null > 1) {
            return Result<OwnedBlock>::Fail(Err::kProtocolError);
        }
        col.ptype = (ptype == RTDB_BLKPT_I64)
                        ? DataType::kInt64
                        : (ptype == RTDB_BLKPT_F64 ? DataType::kFloat64 : DataType::kUtf8Var);
        if (has_null != 0) {
            if (r.remaining() < n_rows) return Result<OwnedBlock>::Fail(Err::kProtocolError);
            col.null_flags.resize(n_rows);
            if (!r.Raw(col.null_flags.data(), n_rows)) {
                return Result<OwnedBlock>::Fail(Err::kProtocolError);
            }
        }
        if (ptype == RTDB_BLKPT_I64) {
            if (r.remaining() < static_cast<size_t>(n_rows) * 8u) {
                return Result<OwnedBlock>::Fail(Err::kProtocolError);
            }
            col.i64.resize(n_rows);
            for (uint32_t k = 0; k < n_rows; ++k) col.i64[k] = r.I64();
        } else if (ptype == RTDB_BLKPT_F64) {
            if (r.remaining() < static_cast<size_t>(n_rows) * 8u) {
                return Result<OwnedBlock>::Fail(Err::kProtocolError);
            }
            col.f64.resize(n_rows);
            for (uint32_t k = 0; k < n_rows; ++k) col.f64[k] = r.F64();
        } else {
            // 变长列每行至少 4B（u32 长度前缀），以此设分配下限防 OOM。
            if (r.remaining() < static_cast<size_t>(n_rows) * 4u) {
                return Result<OwnedBlock>::Fail(Err::kProtocolError);
            }
            col.var.reserve(n_rows);
            for (uint32_t k = 0; k < n_rows; ++k) {
                std::string s;
                if (!r.Var(&s)) return Result<OwnedBlock>::Fail(Err::kProtocolError);
                col.var.push_back(std::move(s));
            }
        }
        out.cols.push_back(std::move(col));
    }
    if (!r.ok()) return Result<OwnedBlock>::Fail(Err::kProtocolError);

    // 构建内部视图（cols 已定容，地址稳定）。
    for (OwnedBlock::Col& col : out.cols) {
        if (col.ptype == DataType::kUtf8Var) {
            col.var_views.reserve(col.var.size());
            for (const std::string& s : col.var) col.var_views.push_back(std::string_view(s));
        }
    }
    out.views.reserve(out.cols.size());
    for (OwnedBlock::Col& col : out.cols) {
        BlockColumn bc;
        bc.name = col.name;
        bc.type = col.ptype;
        if (col.ptype == DataType::kInt64) {
            bc.data = col.i64.data();
        } else if (col.ptype == DataType::kFloat64) {
            bc.data = col.f64.data();
        } else {
            bc.data = col.var_views.data();
        }
        bc.null_flags = col.null_flags.empty() ? nullptr : col.null_flags.data();
        bc.rows = out.row_count;
        out.views.push_back(bc);
    }
    return Result<OwnedBlock>::Ok(std::move(out));
}

RecordBlockView OwnedBlock::View() const noexcept {
    return RecordBlockView{std::string_view(table),
                           Span<const BlockColumn>(views.data(), views.size()), row_count};
}

}  // namespace net
}  // namespace rtdb
