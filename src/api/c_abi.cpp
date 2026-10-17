/// \file
/// \brief C ABI 导出面（docs/04 §4，M2-W3；docs/10 §2-M2-D3）。
///
/// 设计纪律：
///  - **薄封装**：参数解码 → 调 C++ 门面 → 错误码映射，无独立状态机；
///  - **不抛异常跨界**：每个入口 try/catch(...) 兜底为 RTDB_ERR_INTERNAL；
///  - **所有权**：引擎/表句柄由本库 new/delete；get/scan 结果由本库
///    malloc，调用方经 free_row/free_rows 归还（C 语义）。
///
/// 句柄定义在此文件（不透明类型的具体化）；C 头只前置声明。

#include "rtdb/rtdb_cabi.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "rtdb/rtdb.hpp"

// ---- 不透明句柄的具体化（仅本 TU 可见）----
struct rtdb_engine {
    rtdb::Engine* engine;
};

struct rtdb_table {
    rtdb::TableHandle handle;
};

namespace {

using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Err;
using rtdb::FsyncPolicy;
using rtdb::Role;

inline int32_t Code(Err e) noexcept { return static_cast<int32_t>(e); }

bool IsKnownType(int32_t t) noexcept { return t >= RTDB_T_INT8 && t <= RTDB_T_UNIXTSNS; }

bool IsVarType(int32_t t) noexcept { return t == RTDB_T_UTF8VAR || t == RTDB_T_BYTESVAR; }

bool IsFloatType(int32_t t) noexcept { return t == RTDB_T_FLOAT32 || t == RTDB_T_FLOAT64; }

/// C 值 → ColumnValue（未知类型由调用方先行拒绝；此处兜底为 Null）。
ColumnValue FromCValue(const rtdb_value& v) {
    if (v.is_null) return ColumnValue::Null();
    if (IsVarType(v.type)) {
        const std::string s = v.bytes != nullptr ? std::string(v.bytes, v.bytes_len) : std::string();
        return v.type == RTDB_T_BYTESVAR ? ColumnValue::Bytes(s) : ColumnValue::Text(s);
    }
    if (IsFloatType(v.type)) return ColumnValue::Real(v.real_value);
    return ColumnValue::IntOf(static_cast<DataType>(v.type), v.int_value);
}

/// 主键值（仅整数类合法）。
bool FromCKey(const rtdb_value* key, ColumnValue* out) noexcept {
    if (key == nullptr || out == nullptr || key->is_null) return false;
    if (!IsKnownType(key->type) || IsFloatType(key->type) || IsVarType(key->type)) return false;
    *out = ColumnValue::IntOf(static_cast<DataType>(key->type), key->int_value);
    return true;
}

/// 填充单行结果（vals 由 calloc 保证零初始化，bytes 全空串）。
/// 失败时 out->nvals 仍为 src.size()，调用方可安全 free_row 回收已分配前缀。
int32_t FillRow(const std::vector<ColumnValue>& src, rtdb_row* out) {
    out->vals = nullptr;
    out->nvals = 0;
    if (src.empty()) return RTDB_OK;
    auto* vals = static_cast<rtdb_value*>(std::calloc(src.size(), sizeof(rtdb_value)));
    if (vals == nullptr) return RTDB_ERR_OUT_OF_MEMORY;
    out->vals = vals;
    out->nvals = src.size();
    for (size_t i = 0; i < src.size(); ++i) {
        const ColumnValue& cv = src[i];
        rtdb_value& v = vals[i];
        v.type = static_cast<int32_t>(cv.type());
        v.is_null = cv.is_null() ? 1 : 0;
        if (cv.is_null()) continue;
        switch (cv.type()) {
            case DataType::kFloat32:
            case DataType::kFloat64:
                v.real_value = cv.as_real();
                break;
            case DataType::kUtf8Var:
            case DataType::kBytesVar: {
                const std::string& b = cv.as_bytes();
                if (!b.empty()) {
                    auto* buf = static_cast<char*>(std::malloc(b.size()));
                    if (buf == nullptr) return RTDB_ERR_OUT_OF_MEMORY;
                    std::memcpy(buf, b.data(), b.size());
                    v.bytes = buf;
                    v.bytes_len = static_cast<uint32_t>(b.size());
                }
                break;
            }
            default:
                v.int_value = cv.as_int();
                break;
        }
    }
    return RTDB_OK;
}

}  // namespace

extern "C" {

uint32_t rtdb_abi_version(void) { return RTDB_ABI_VER; }

int32_t rtdb_abi_open(const char* instance, const char* path, uint32_t role, uint32_t wal_fsync,
                      rtdb_engine** out) {
    if (instance == nullptr || path == nullptr || out == nullptr)
        return RTDB_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (role > RTDB_ROLE_READER || wal_fsync > RTDB_FSYNC_ALWAYS) return RTDB_ERR_INVALID_ARGUMENT;
    try {
        rtdb::Options opts;
        opts.instance = instance;
        opts.path = path;
        opts.role = static_cast<Role>(role);
        opts.wal_fsync = static_cast<FsyncPolicy>(wal_fsync);
        auto res = rtdb::Engine::Open(opts);
        if (!res.IsOk()) return Code(res.Error());
        auto* h = new (std::nothrow) rtdb_engine;
        if (h == nullptr) return RTDB_ERR_OUT_OF_MEMORY;
        h->engine = res.TakeValue().release();
        *out = h;
        return RTDB_OK;
    } catch (...) {
        return RTDB_ERR_INTERNAL;
    }
}

void rtdb_abi_close(rtdb_engine* engine) {
    if (engine == nullptr) return;
    delete engine->engine;
    delete engine;
}

int64_t rtdb_abi_committed_seq(rtdb_engine* engine) {
    if (engine == nullptr || engine->engine == nullptr) return -1;
    return engine->engine->CommittedSeq();
}

int32_t rtdb_abi_create_table(rtdb_engine* engine, const char* name, const rtdb_column_def* cols,
                              size_t ncols, uint32_t max_rows_hint, rtdb_table** out) {
    if (engine == nullptr || engine->engine == nullptr || name == nullptr || cols == nullptr ||
        ncols == 0 || out == nullptr)
        return RTDB_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    for (size_t i = 0; i < ncols; ++i) {
        if (cols[i].name == nullptr || !IsKnownType(cols[i].type)) return RTDB_ERR_INVALID_ARGUMENT;
    }
    try {
        rtdb::TableDef def;
        def.name = name;
        def.columns.reserve(ncols);
        for (size_t i = 0; i < ncols; ++i) {
            rtdb::ColumnDef col;
            col.name = cols[i].name;
            col.type = static_cast<DataType>(cols[i].type);
            col.len = cols[i].len;
            col.nullable = cols[i].nullable != 0;
            def.columns.push_back(std::move(col));
        }
        def.max_rows_hint = max_rows_hint;
        auto res = engine->engine->CreateTable(def);
        if (!res.IsOk()) return Code(res.Error());
        auto* t = new (std::nothrow) rtdb_table;
        if (t == nullptr) return RTDB_ERR_OUT_OF_MEMORY;
        t->handle = res.TakeValue();
        *out = t;
        return RTDB_OK;
    } catch (...) {
        return RTDB_ERR_INTERNAL;
    }
}

int32_t rtdb_abi_open_table(rtdb_engine* engine, const char* name, rtdb_table** out) {
    if (engine == nullptr || engine->engine == nullptr || name == nullptr || out == nullptr)
        return RTDB_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    try {
        auto res = engine->engine->OpenTable(name);
        if (!res.IsOk()) return Code(res.Error());
        auto* t = new (std::nothrow) rtdb_table;
        if (t == nullptr) return RTDB_ERR_OUT_OF_MEMORY;
        t->handle = res.TakeValue();
        *out = t;
        return RTDB_OK;
    } catch (...) {
        return RTDB_ERR_INTERNAL;
    }
}

int32_t rtdb_abi_drop_table(rtdb_engine* engine, const char* name) {
    if (engine == nullptr || engine->engine == nullptr || name == nullptr)
        return RTDB_ERR_INVALID_ARGUMENT;
    try {
        auto res = engine->engine->DropTable(name);
        return res.IsOk() ? RTDB_OK : Code(res.Error());
    } catch (...) {
        return RTDB_ERR_INTERNAL;
    }
}

void rtdb_abi_close_table(rtdb_table* table) { delete table; }

int32_t rtdb_abi_insert(rtdb_engine* engine, rtdb_table* table, const rtdb_value* vals,
                        size_t nvals, uint64_t* out_row_no) {
    if (out_row_no != nullptr) *out_row_no = 0;
    if (engine == nullptr || engine->engine == nullptr || table == nullptr || vals == nullptr ||
        nvals == 0)
        return RTDB_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < nvals; ++i) {
        if (!vals[i].is_null && !IsKnownType(vals[i].type)) return RTDB_ERR_INVALID_ARGUMENT;
    }
    try {
        std::vector<ColumnValue> row;
        row.reserve(nvals);
        for (size_t i = 0; i < nvals; ++i) row.push_back(FromCValue(vals[i]));
        auto res = engine->engine->Insert(table->handle, row);
        if (!res.IsOk()) return Code(res.Error());
        if (out_row_no != nullptr) *out_row_no = res.Value();
        return RTDB_OK;
    } catch (...) {
        return RTDB_ERR_INTERNAL;
    }
}

int32_t rtdb_abi_get(rtdb_engine* engine, rtdb_table* table, const rtdb_value* key,
                     rtdb_row* out) {
    if (out != nullptr) {
        out->vals = nullptr;
        out->nvals = 0;
    }
    if (engine == nullptr || engine->engine == nullptr || table == nullptr || out == nullptr)
        return RTDB_ERR_INVALID_ARGUMENT;
    ColumnValue cv;
    if (!FromCKey(key, &cv)) return RTDB_ERR_INVALID_ARGUMENT;
    try {
        auto res = engine->engine->GetByKey(table->handle, cv);
        if (!res.IsOk()) return Code(res.Error());
        const int32_t rc = FillRow(res.Value(), out);
        if (rc != RTDB_OK) {
            rtdb_abi_free_row(out);
            return rc;
        }
        return RTDB_OK;
    } catch (...) {
        rtdb_abi_free_row(out);
        return RTDB_ERR_INTERNAL;
    }
}

int32_t rtdb_abi_scan(rtdb_engine* engine, rtdb_table* table, const rtdb_value* lo,
                      const rtdb_value* hi, rtdb_rows* out) {
    if (out != nullptr) {
        out->rows = nullptr;
        out->nrows = 0;
    }
    if (engine == nullptr || engine->engine == nullptr || table == nullptr || out == nullptr)
        return RTDB_ERR_INVALID_ARGUMENT;
    rtdb::Engine::KeyRange range;
    if (lo != nullptr) {
        ColumnValue lv;
        if (!FromCKey(lo, &lv)) return RTDB_ERR_INVALID_ARGUMENT;
        range.lo = lv;
    }
    if (hi != nullptr) {
        ColumnValue hv;
        if (!FromCKey(hi, &hv)) return RTDB_ERR_INVALID_ARGUMENT;
        range.hi = hv;
    }
    try {
        auto res = engine->engine->Scan(table->handle, range);
        if (!res.IsOk()) return Code(res.Error());
        const auto& src = res.Value();
        if (src.empty()) return RTDB_OK;
        auto* rows = static_cast<rtdb_row*>(std::calloc(src.size(), sizeof(rtdb_row)));
        if (rows == nullptr) return RTDB_ERR_OUT_OF_MEMORY;
        out->rows = rows;
        out->nrows = src.size();
        for (size_t i = 0; i < src.size(); ++i) {
            const int32_t rc = FillRow(src[i], &rows[i]);
            if (rc != RTDB_OK) {
                rtdb_abi_free_rows(out);
                return rc;
            }
        }
        return RTDB_OK;
    } catch (...) {
        rtdb_abi_free_rows(out);
        return RTDB_ERR_INTERNAL;
    }
}

int32_t rtdb_abi_upsert_block(rtdb_engine* engine, const char* table_name,
                              const rtdb_column* cols, size_t ncols, size_t rowcount,
                              uint64_t* out_committed_seq) {
    if (out_committed_seq != nullptr) *out_committed_seq = 0;
    if (engine == nullptr || engine->engine == nullptr || table_name == nullptr ||
        cols == nullptr || ncols == 0 || rowcount == 0)
        return RTDB_ERR_INVALID_ARGUMENT;
    try {
        // 变长列的 string_view 视图（零拷贝：指向调用方缓冲，仅本调用有效）。
        std::vector<std::vector<std::string_view>> var_views(ncols);
        std::vector<rtdb::BlockColumn> bcols(ncols);
        for (size_t c = 0; c < ncols; ++c) {
            const rtdb_column& in = cols[c];
            if (in.name == nullptr || in.data == nullptr) return RTDB_ERR_INVALID_ARGUMENT;
            rtdb::BlockColumn& bc = bcols[c];
            bc.name = in.name;
            bc.null_flags = in.null_flags;
            bc.rows = rowcount;
            switch (in.type) {
                case RTDB_T_INT64:
                    bc.type = DataType::kInt64;
                    bc.data = in.data;
                    break;
                case RTDB_T_FLOAT64:
                    bc.type = DataType::kFloat64;
                    bc.data = in.data;
                    break;
                case RTDB_T_UTF8VAR:
                case RTDB_T_BYTESVAR: {
                    if (in.lengths == nullptr) return RTDB_ERR_INVALID_ARGUMENT;
                    const auto* ptrs = static_cast<const char* const*>(in.data);
                    auto& vs = var_views[c];
                    vs.reserve(rowcount);
                    for (size_t r = 0; r < rowcount; ++r) {
                        if (ptrs[r] == nullptr) {
                            vs.emplace_back("", 0);
                        } else {
                            vs.emplace_back(ptrs[r], in.lengths[r]);
                        }
                    }
                    bc.type = DataType::kUtf8Var;
                    bc.data = vs.data();
                    break;
                }
                default:
                    return RTDB_ERR_INVALID_ARGUMENT;
            }
        }
        rtdb::RecordBlockView view;
        view.table_name = table_name;
        view.columns = rtdb::Span<const rtdb::BlockColumn>(bcols);
        view.row_count = rowcount;
        auto res = engine->engine->UpsertBatch(view);
        if (!res.IsOk()) return Code(res.Error());
        if (out_committed_seq != nullptr) *out_committed_seq = res.Value();
        return RTDB_OK;
    } catch (...) {
        return RTDB_ERR_INTERNAL;
    }
}

void rtdb_abi_free_row(rtdb_row* row) {
    if (row == nullptr) return;
    if (row->vals != nullptr) {
        for (size_t i = 0; i < row->nvals; ++i) {
            // bytes 为库内 malloc 缓冲（常量视图仅在本文件产生）。
            std::free(const_cast<char*>(row->vals[i].bytes));
        }
        std::free(row->vals);
    }
    row->vals = nullptr;
    row->nvals = 0;
}

void rtdb_abi_free_rows(rtdb_rows* rows) {
    if (rows == nullptr) return;
    if (rows->rows != nullptr) {
        for (size_t i = 0; i < rows->nrows; ++i) rtdb_abi_free_row(&rows->rows[i]);
        std::free(rows->rows);
    }
    rows->rows = nullptr;
    rows->nrows = 0;
}

}  // extern "C"
