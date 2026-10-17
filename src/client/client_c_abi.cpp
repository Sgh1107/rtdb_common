/// rtdb_client C ABI 实现（docs/13 §4，M3-W4）：
/// 连接/PING/建表/删表/块写/扫描/订阅回调；值编解码复用 C++ 客户端 API。
///
/// 内存约定：扫描结果 = 单块连续分配 [内部 owner 指针][rtdb_row 数组]，
/// 释放时由 `rtdb_client_free_rows` 经首行回退取回 owner 统一回收；
/// 订阅回调的结构仅在回调期间有效（泵线程栈上物化）。

#include "rtdb/rtdb_client_c.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rtdb/rtdb_client.hpp"

namespace {

using rtdb::ColumnValue;
using rtdb::DataType;

/// C 值 → C++ 值（空值/类型直映射；未知类型按整数承载）。
ColumnValue FromCValue(const rtdb_value& v) {
    if (v.is_null != 0) return ColumnValue::Null();
    const DataType t = static_cast<DataType>(v.type);
    switch (t) {
        case DataType::kFloat32:
        case DataType::kFloat64:
            return ColumnValue::Real(v.real_value);
        case DataType::kUtf8Var:
            return ColumnValue::Text(std::string(v.bytes != nullptr ? v.bytes : "", v.bytes_len));
        case DataType::kBytesVar:
            return ColumnValue::Bytes(std::string(v.bytes != nullptr ? v.bytes : "", v.bytes_len));
        default:
            return ColumnValue::IntOf(t, v.int_value);
    }
}

/// C++ 值 → C 值；变长内容拷入 blobs（deque 引用稳定，指针随结果存活）。
void FillCValue(const ColumnValue& v, rtdb_value* out, std::deque<std::string>* blobs) {
    out->type = static_cast<int32_t>(v.type());
    out->is_null = v.is_null() ? 1 : 0;
    out->int_value = 0;
    out->real_value = 0.0;
    out->bytes = nullptr;
    out->bytes_len = 0;
    if (v.is_null()) return;
    switch (v.type()) {
        case DataType::kFloat32:
        case DataType::kFloat64:
            out->real_value = v.as_real();
            break;
        case DataType::kUtf8Var:
        case DataType::kBytesVar:
            blobs->emplace_back(v.as_bytes());
            out->bytes = blobs->back().c_str();
            out->bytes_len = static_cast<uint32_t>(blobs->back().size());
            break;
        default:
            out->int_value = v.as_int();
            break;
    }
}

/// 扫描结果 owner（经 RowsHeader 指针间接持有）。
struct RowsOwner {
    std::deque<std::string> blobs;  ///< 变长内容存储（引用稳定）
    std::vector<std::vector<rtdb_value>> vals;
};

/// rtdb_rows.rows 之前的内嵌头：回退 sizeof 即取回 owner。
struct RowsHeader {
    RowsOwner* owner;
};

void FreeRows(rtdb_rows* rows) {
    if (rows == nullptr) return;
    if (rows->rows != nullptr) {
        char* base = reinterpret_cast<char*>(rows->rows) - sizeof(RowsHeader);
        RowsHeader* hdr = reinterpret_cast<RowsHeader*>(base);
        delete hdr->owner;
        std::free(hdr);
    }
    rows->rows = nullptr;
    rows->nrows = 0;
}

bool MarshalRows(const std::vector<std::vector<ColumnValue>>& src, rtdb_rows* out) {
    out->rows = nullptr;
    out->nrows = 0;
    if (src.empty()) return true;
    std::unique_ptr<RowsOwner> owner(new RowsOwner());
    owner->vals.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        owner->vals[i].resize(src[i].size());
        for (size_t j = 0; j < src[i].size(); ++j) {
            FillCValue(src[i][j], &owner->vals[i][j], &owner->blobs);
        }
    }
    void* block = std::malloc(sizeof(RowsHeader) + src.size() * sizeof(rtdb_row));
    if (block == nullptr) return false;
    RowsHeader* hdr = reinterpret_cast<RowsHeader*>(block);
    hdr->owner = owner.release();
    rtdb_row* arr = reinterpret_cast<rtdb_row*>(static_cast<char*>(block) + sizeof(RowsHeader));
    for (size_t i = 0; i < src.size(); ++i) {
        arr[i].vals = hdr->owner->vals[i].data();
        arr[i].nvals = src[i].size();
    }
    out->rows = arr;
    out->nrows = src.size();
    return true;
}

/// 变更事件物化（泵线程栈上；回调返回即失效）。
struct ChangeStorage {
    std::deque<std::string> blobs;
    std::vector<rtdb_value> vals;
    rtdb_change chg{};
};

void MarshalChange(const rtdb::ChangeEvent& ev, ChangeStorage* st) {
    switch (ev.kind) {
        case rtdb::ChangeEvent::Kind::kInsert:
            st->chg.kind = RTDB_CHG_INSERT;
            break;
        case rtdb::ChangeEvent::Kind::kUpdate:
            st->chg.kind = RTDB_CHG_UPDATE;
            break;
        case rtdb::ChangeEvent::Kind::kDelete:
            st->chg.kind = RTDB_CHG_DELETE;
            break;
    }
    st->chg.table_id = ev.table_id;
    st->blobs.emplace_back(ev.table);
    st->chg.table = st->blobs.back().c_str();
    st->chg.seq = ev.seq;
    st->chg.ts_ns = ev.ts_ns;
    st->chg.row_no = ev.row_no;
    st->chg.row_ver = ev.row_ver;
    st->chg.row.vals = nullptr;
    st->chg.row.nvals = 0;
    st->chg.key = rtdb_value{};
    if (ev.kind == rtdb::ChangeEvent::Kind::kDelete) {
        FillCValue(ev.key, &st->chg.key, &st->blobs);
    } else {
        st->vals.resize(ev.row.size());
        for (size_t i = 0; i < ev.row.size(); ++i) {
            FillCValue(ev.row[i], &st->vals[i], &st->blobs);
        }
        st->chg.row.vals = st->vals.data();
        st->chg.row.nvals = ev.row.size();
    }
}

/// C 列类型 → 块载荷类型（INT64 / FLOAT64 / VAR 三族，docs/13 §3-D5）。
DataType PayloadOf(int32_t type) {
    switch (type) {
        case RTDB_T_FLOAT32:
        case RTDB_T_FLOAT64:
            return DataType::kFloat64;
        case RTDB_T_UTF8VAR:
        case RTDB_T_BYTESVAR:
            return DataType::kUtf8Var;
        default:
            return DataType::kInt64;
    }
}

}  // namespace

// ============================================================
// C ABI：连接与请求面
// ============================================================

struct rtdb_client {
    std::unique_ptr<rtdb::Client> c;
};

struct rtdb_client_sub {
    std::unique_ptr<rtdb::Subscription> sub;
};

extern "C" {

int32_t rtdb_client_connect(const rtdb_client_options* opts, rtdb_client** out) {
    if (opts == nullptr || out == nullptr || opts->port == 0) {
        return RTDB_ERR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    rtdb::ClientOptions o;
    o.host = opts->host != nullptr ? opts->host : "127.0.0.1";
    o.port = opts->port;
    o.user = opts->user != nullptr ? opts->user : "";
    o.password = opts->password != nullptr ? opts->password : "";
    o.auth_salt = opts->auth_salt != nullptr ? opts->auth_salt : "";
    if (opts->connect_timeout_ms != 0) o.connect_timeout_ms = opts->connect_timeout_ms;
    if (opts->request_timeout_ms != 0) o.request_timeout_ms = opts->request_timeout_ms;

    auto r = rtdb::Client::Connect(o);
    if (!r.IsOk()) return static_cast<int32_t>(r.Error());
    std::unique_ptr<rtdb_client> handle(new rtdb_client());
    handle->c = r.TakeValue();
    *out = handle.release();
    return RTDB_OK;
}

void rtdb_client_close(rtdb_client* c) { delete c; }

int32_t rtdb_client_ping(rtdb_client* c, int64_t* committed_seq, uint64_t* heartbeat_ns,
                         uint32_t* ttl_ms) {
    if (c == nullptr || c->c == nullptr) return RTDB_ERR_INVALID_ARGUMENT;
    auto r = c->c->Ping();
    if (!r.IsOk()) return static_cast<int32_t>(r.Error());
    if (committed_seq != nullptr) *committed_seq = r.Value().committed_seq;
    if (heartbeat_ns != nullptr) *heartbeat_ns = r.Value().writer_heartbeat_ns;
    if (ttl_ms != nullptr) *ttl_ms = r.Value().session_ttl_ms;
    return RTDB_OK;
}

int32_t rtdb_client_create_table(rtdb_client* c, const char* name,
                                 const rtdb_column_def* cols, size_t ncols,
                                 uint32_t max_rows_hint) {
    if (c == nullptr || c->c == nullptr || name == nullptr || cols == nullptr || ncols == 0) {
        return RTDB_ERR_INVALID_ARGUMENT;
    }
    rtdb::TableDef def;
    def.name = name;
    def.columns.reserve(ncols);
    for (size_t i = 0; i < ncols; ++i) {
        rtdb::ColumnDef cd;
        cd.name = cols[i].name != nullptr ? cols[i].name : "";
        cd.type = static_cast<DataType>(cols[i].type);
        cd.len = cols[i].len;
        cd.nullable = cols[i].nullable != 0;
        def.columns.push_back(std::move(cd));
    }
    def.row_cap = max_rows_hint;
    auto r = c->c->CreateTable(def);
    if (!r.IsOk()) return static_cast<int32_t>(r.Error());
    return RTDB_OK;
}

int32_t rtdb_client_drop_table(rtdb_client* c, const char* name) {
    if (c == nullptr || c->c == nullptr || name == nullptr) return RTDB_ERR_INVALID_ARGUMENT;
    auto r = c->c->DropTable(name);
    if (!r.IsOk()) return static_cast<int32_t>(r.Error());
    return RTDB_OK;
}

int32_t rtdb_client_upsert_block(rtdb_client* c, const char* table, const rtdb_column* cols,
                                 size_t ncols, size_t rowcount, uint64_t* out_committed_seq) {
    if (c == nullptr || c->c == nullptr || table == nullptr || cols == nullptr) {
        return RTDB_ERR_INVALID_ARGUMENT;
    }
    // 列视图 + 名称/变长视图存储（须覆盖 UpsertBatch 调用）。
    std::vector<std::string_view> names;
    names.reserve(ncols);
    std::vector<std::vector<std::string_view>> var_views(ncols);
    std::vector<rtdb::BlockColumn> blocks;
    blocks.reserve(ncols);
    for (size_t i = 0; i < ncols; ++i) {
        names.emplace_back(cols[i].name != nullptr ? cols[i].name : "");
        rtdb::BlockColumn bc;
        bc.name = names.back();
        bc.type = PayloadOf(cols[i].type);
        bc.null_flags = cols[i].null_flags;
        bc.rows = rowcount;
        if (bc.type == DataType::kUtf8Var) {
            const char* const* ptrs = static_cast<const char* const*>(cols[i].data);
            const uint32_t* lens = cols[i].lengths;
            var_views[i].reserve(rowcount);
            for (size_t r = 0; r < rowcount; ++r) {
                const char* p = (ptrs != nullptr) ? ptrs[r] : nullptr;
                const uint32_t len = (lens != nullptr) ? lens[r] : 0;
                var_views[i].emplace_back(p != nullptr ? std::string_view(p, len)
                                                       : std::string_view());
            }
            bc.data = var_views[i].data();
        } else {
            bc.data = cols[i].data;
        }
        blocks.push_back(bc);
    }
    rtdb::RecordBlockView view;
    view.table_name = table;
    view.columns = rtdb::Span<const rtdb::BlockColumn>(blocks.data(), blocks.size());
    view.row_count = rowcount;
    auto r = c->c->UpsertBatch(view);
    if (!r.IsOk()) return static_cast<int32_t>(r.Error());
    if (out_committed_seq != nullptr) *out_committed_seq = static_cast<uint64_t>(r.Value());
    return RTDB_OK;
}

int32_t rtdb_client_scan(rtdb_client* c, const char* table, const rtdb_value* lo,
                         const rtdb_value* hi, rtdb_rows* out) {
    if (c == nullptr || c->c == nullptr || table == nullptr || out == nullptr) {
        return RTDB_ERR_INVALID_ARGUMENT;
    }
    out->rows = nullptr;
    out->nrows = 0;
    rtdb::Engine::KeyRange range;
    if (lo != nullptr && lo->is_null == 0) range.lo = FromCValue(*lo);
    if (hi != nullptr && hi->is_null == 0) range.hi = FromCValue(*hi);
    auto r = c->c->Scan(table, range);
    if (!r.IsOk()) return static_cast<int32_t>(r.Error());
    if (!MarshalRows(r.Value().rows, out)) return RTDB_ERR_OUT_OF_MEMORY;
    return RTDB_OK;
}

void rtdb_client_free_rows(rtdb_rows* rows) { FreeRows(rows); }

int32_t rtdb_client_subscribe(rtdb_client* c, const char* const* tables, size_t ntables,
                              rtdb_client_change_fn on_change,
                              rtdb_client_overflow_fn on_overflow,
                              rtdb_client_state_fn on_state, void* user,
                              rtdb_client_sub** out) {
    if (c == nullptr || c->c == nullptr || out == nullptr ||
        (ntables != 0 && tables == nullptr)) {
        return RTDB_ERR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    rtdb::SubscriptionOptions o;
    o.tables.reserve(ntables);
    for (size_t i = 0; i < ntables; ++i) {
        if (tables[i] != nullptr) o.tables.emplace_back(tables[i]);
    }
    if (on_change != nullptr) {
        o.on_change = [on_change, user](const rtdb::ChangeEvent& ev) {
            ChangeStorage st;
            MarshalChange(ev, &st);
            on_change(&st.chg, user);
        };
    }
    if (on_overflow != nullptr) {
        o.on_overflow = [on_overflow, user](rtdb::Subscription&) { on_overflow(user); };
    }
    if (on_state != nullptr) {
        o.on_state = [on_state, user](bool connected, uint32_t attempt) {
            on_state(connected ? 1 : 0, attempt, user);
        };
    }
    auto r = c->c->Subscribe(o);
    if (!r.IsOk()) return static_cast<int32_t>(r.Error());
    std::unique_ptr<rtdb_client_sub> handle(new rtdb_client_sub());
    handle->sub = r.TakeValue();
    *out = handle.release();
    return RTDB_OK;
}

void rtdb_client_sub_close(rtdb_client_sub* sub) { delete sub; }

uint64_t rtdb_client_sub_position(const rtdb_client_sub* sub) {
    if (sub == nullptr || sub->sub == nullptr) return 0;
    return sub->sub->position();
}

void rtdb_client_sub_reset(rtdb_client_sub* sub, uint64_t seq) {
    if (sub == nullptr || sub->sub == nullptr) return;
    sub->sub->ResetTo(seq);
}

}  // extern "C"
