/// \file
/// \brief M4-W3 老系统迁移核心（docs/16 D8）：零三方依赖的 JSON 子集解析、
/// 源映像适配器、表映射、原子批迁移与校验报告。
///
/// 真实 PSRtdb 二进制映像格式不可得，本版交付**自描述同构 fixture 适配器**
/// （FixtureAdapter）。接入真实老映像时只需实现 IPsrImageAdapter：
/// `Tables()` 返回表名/列定义/行值（ColumnValue），其余迁移/校验/报告链路
/// 无需改动；这一限制会同时写进迁移报告，避免被误读为已验证真实格式。
///
/// 共享内存布局、C ABI 均不变；迁移只经 Engine 本地 API（CreateTable /
/// ExecuteBatch / ScanAll / CreateSnapshot），不引入新的持久化语义。

#ifndef RTDB_TOOLS_MIGRATE_CORE_HPP_
#define RTDB_TOOLS_MIGRATE_CORE_HPP_

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rtdb/column_value.hpp"
#include "rtdb/engine.hpp"
#include "rtdb/err.hpp"
#include "rtdb/options.hpp"
#include "rtdb/result.hpp"
#include "rtdb/table_def.hpp"

namespace rtdb {
namespace migrate {

// ---------------------------------------------------------------------------
// 最小 JSON 子集（对象/数组/字符串/数字/bool/null），带字符偏移诊断
// ---------------------------------------------------------------------------
struct JVal {
    enum class T { kNull, kBool, kNum, kStr, kArr, kObj };
    T type = T::kNull;
    bool boolean = false;
    double number = 0.0;
    std::string text;  ///< 字符串内容 / 数字原文
    std::vector<JVal> arr;
    std::map<std::string, JVal> obj;

    const JVal* Find(std::string_view key) const {
        if (type != T::kObj) return nullptr;
        const auto it = obj.find(std::string(key));
        return it == obj.end() ? nullptr : &it->second;
    }
};

class Json {
public:
    explicit Json(std::string_view s) : s_(s) {}

    bool Parse(JVal* out, std::string* err) {
        if (!Value(out)) return Fail(err);
        Space();
        if (pos_ != s_.size()) return Fail(err, "trailing characters");
        return true;
    }

private:
    bool Fail(std::string* err, const char* what = "malformed JSON") const {
        if (err != nullptr)
            *err = std::string(what) + " at offset " + std::to_string(pos_);
        return false;
    }
    void Space() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos_;
            else break;
        }
    }
    bool Literal(std::string_view lit) {
        if (s_.compare(pos_, lit.size(), lit) != 0) return false;
        pos_ += lit.size();
        return true;
    }
    static void AppendUtf8(uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool Hex4(uint32_t* out) {
        if (pos_ + 4 > s_.size()) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        *out = v;
        return true;
    }
    bool Str(std::string* out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        out->clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!Hex4(&cp)) return false;
                    AppendUtf8(cp, out);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Num(JVal* v) {
        const size_t begin = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ == begin) return false;
        v->type = JVal::T::kNum;
        v->text.assign(s_.substr(begin, pos_ - begin));
        v->number = std::strtod(v->text.c_str(), nullptr);
        return true;
    }
    bool Object(JVal* v) {
        ++pos_;  // '{'
        v->type = JVal::T::kObj;
        Space();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            Space();
            std::string key;
            if (!Str(&key)) return false;
            Space();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            JVal child;
            if (!Value(&child)) return false;
            v->obj[key] = std::move(child);
            Space();
            if (pos_ >= s_.size()) return false;
            const char c = s_[pos_++];
            if (c == '}') return true;
            if (c != ',') return false;
        }
    }
    bool Array(JVal* v) {
        ++pos_;  // '['
        v->type = JVal::T::kArr;
        Space();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            JVal child;
            if (!Value(&child)) return false;
            v->arr.push_back(std::move(child));
            Space();
            if (pos_ >= s_.size()) return false;
            const char c = s_[pos_++];
            if (c == ']') return true;
            if (c != ',') return false;
        }
    }
    bool Value(JVal* v) {
        Space();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') return Object(v);
        if (c == '[') return Array(v);
        if (c == '"') { v->type = JVal::T::kStr; return Str(&v->text); }
        if (c == 't' && Literal("true")) { v->type = JVal::T::kBool; v->boolean = true; return true; }
        if (c == 'f' && Literal("false")) { v->type = JVal::T::kBool; v->boolean = false; return true; }
        if (c == 'n' && Literal("null")) { v->type = JVal::T::kNull; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return Num(v);
        return false;
    }

    std::string_view s_;
    size_t pos_ = 0;
};

inline bool ReadFile(const std::string& path, std::string* out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open file: " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}
inline bool WriteFile(const std::string& path, const std::string& data, std::string* err) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write file: " + path; return false; }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}
inline std::string Quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    out += '"';
    return out;
}

// ---------------------------------------------------------------------------
// 类型 / 源映像 / 映射
// ---------------------------------------------------------------------------
inline bool TypeOf(std::string_view s, DataType* out) {
    if (s == "int8") *out = DataType::kInt8;
    else if (s == "int16") *out = DataType::kInt16;
    else if (s == "int32") *out = DataType::kInt32;
    else if (s == "int64") *out = DataType::kInt64;
    else if (s == "bool") *out = DataType::kBool;
    else if (s == "float32") *out = DataType::kFloat32;
    else if (s == "float64") *out = DataType::kFloat64;
    else if (s == "utf8") *out = DataType::kUtf8Var;
    else if (s == "bytes") *out = DataType::kBytesVar;
    else if (s == "ts") *out = DataType::kUnixTsNs;
    else return false;
    return true;
}

inline bool ToValue(const JVal& j, DataType t, ColumnValue* out, std::string* err) {
    const char* bad = "value type does not match declared column type";
    if (j.type == JVal::T::kNull) { *out = ColumnValue::Null(); return true; }
    if (t == DataType::kUtf8Var || t == DataType::kBytesVar) {
        if (j.type != JVal::T::kStr) { if (err) *err = bad; return false; }
        *out = t == DataType::kBytesVar ? ColumnValue::Bytes(j.text)
                                        : ColumnValue::Text(j.text);
        return true;
    }
    if (t == DataType::kFloat32 || t == DataType::kFloat64) {
        if (j.type == JVal::T::kNum) *out = ColumnValue::Real(j.number);
        else if (j.type == JVal::T::kStr) *out = ColumnValue::Real(std::strtod(j.text.c_str(), nullptr));
        else { if (err) *err = bad; return false; }
        return true;
    }
    int64_t iv = 0;
    if (j.type == JVal::T::kNum) iv = static_cast<int64_t>(j.number);
    else if (j.type == JVal::T::kBool) iv = j.boolean ? 1 : 0;
    else if (j.type == JVal::T::kStr) iv = std::strtoll(j.text.c_str(), nullptr, 10);
    else { if (err) *err = bad; return false; }
    *out = ColumnValue::IntOf(t, iv);
    return true;
}

struct SrcColumn {
    std::string name;
    DataType type = DataType::kInt64;
    bool nullable = true;
};
struct SrcTable {
    std::string name;
    std::vector<SrcColumn> columns;
    std::vector<std::vector<ColumnValue>> rows;
};

/// 源映像抽象：接真实 PSRtdb 时实现本接口（表列举 + 行游标），迁移链路不变。
class IPsrImageAdapter {
public:
    virtual ~IPsrImageAdapter() = default;
    virtual bool Tables(std::vector<SrcTable>* out, std::string* err) = 0;
};

/// 自描述 fixture 适配器（真实老映像不可得时的默认实现）。
class FixtureAdapter : public IPsrImageAdapter {
public:
    explicit FixtureAdapter(std::string path) : path_(std::move(path)) {}

    bool Tables(std::vector<SrcTable>* out, std::string* err) override {
        std::string text;
        if (!ReadFile(path_, &text, err)) return false;
        JVal root;
        Json p(text);
        if (!p.Parse(&root, err)) return false;
        const JVal* tables = root.Find("tables");
        if (tables == nullptr || tables->type != JVal::T::kArr) {
            if (err) *err = "fixture: missing tables array";
            return false;
        }
        for (const JVal& t : tables->arr) {
            const JVal* name = t.Find("name");
            const JVal* cols = t.Find("columns");
            const JVal* rows = t.Find("rows");
            if (name == nullptr || name->type != JVal::T::kStr || cols == nullptr ||
                cols->type != JVal::T::kArr || rows == nullptr || rows->type != JVal::T::kArr) {
                if (err) *err = "fixture: malformed table entry";
                return false;
            }
            SrcTable st;
            st.name = name->text;
            for (const JVal& c : cols->arr) {
                const JVal* cn = c.Find("name");
                const JVal* ct = c.Find("type");
                if (cn == nullptr || ct == nullptr || cn->type != JVal::T::kStr ||
                    ct->type != JVal::T::kStr) {
                    if (err) *err = "fixture: malformed column entry";
                    return false;
                }
                SrcColumn sc;
                sc.name = cn->text;
                if (!TypeOf(ct->text, &sc.type)) {
                    if (err) *err = "fixture: unknown column type " + ct->text;
                    return false;
                }
                const JVal* nn = c.Find("nullable");
                if (nn != nullptr && nn->type == JVal::T::kBool) sc.nullable = nn->boolean;
                st.columns.push_back(std::move(sc));
            }
            for (const JVal& r : rows->arr) {
                if (r.type != JVal::T::kArr || r.arr.size() != st.columns.size()) {
                    if (err) *err = "fixture: row width mismatch in table " + st.name;
                    return false;
                }
                std::vector<ColumnValue> row;
                for (size_t i = 0; i < r.arr.size(); ++i) {
                    ColumnValue v;
                    if (!ToValue(r.arr[i], st.columns[i].type, &v, err)) return false;
                    row.push_back(std::move(v));
                }
                st.rows.push_back(std::move(row));
            }
            out->push_back(std::move(st));
        }
        return true;
    }

private:
    std::string path_;
};

struct ColumnMap {
    std::string src;
    std::string dst;
    DataType type = DataType::kInt64;
    bool nullable = true;
    std::string def;
    bool has_default = false;
};
struct TableMap {
    std::string src;
    std::string dst;
    std::string primary_key;
    std::vector<ColumnMap> columns;
};
struct Mapping {
    std::vector<TableMap> tables;
};

inline bool ParseMapping(const std::string& text, Mapping* out, std::string* err) {
    JVal root;
    Json p(text);
    if (!p.Parse(&root, err)) return false;
    const JVal* tables = root.Find("tables");
    if (tables == nullptr || tables->type != JVal::T::kArr) {
        if (err) *err = "mapping: missing tables array";
        return false;
    }
    for (const JVal& t : tables->arr) {
        const JVal* src = t.Find("source");
        const JVal* dst = t.Find("target");
        const JVal* cols = t.Find("columns");
        if (src == nullptr || dst == nullptr || cols == nullptr ||
            src->type != JVal::T::kStr || dst->type != JVal::T::kStr ||
            cols->type != JVal::T::kArr || cols->arr.empty()) {
            if (err) *err = "mapping: malformed table entry";
            return false;
        }
        TableMap tm;
        tm.src = src->text;
        tm.dst = dst->text;
        const JVal* pk = t.Find("primary_key");
        if (pk != nullptr && pk->type == JVal::T::kStr) tm.primary_key = pk->text;
        const JVal* defaults = t.Find("defaults");
        for (const JVal& c : cols->arr) {
            const JVal* cs = c.Find("source");
            const JVal* cd = c.Find("target");
            const JVal* ct = c.Find("type");
            if (cs == nullptr || cd == nullptr || ct == nullptr ||
                cs->type != JVal::T::kStr || cd->type != JVal::T::kStr ||
                ct->type != JVal::T::kStr) {
                if (err) *err = "mapping: malformed column entry";
                return false;
            }
            ColumnMap cm;
            cm.src = cs->text;
            cm.dst = cd->text;
            if (!TypeOf(ct->text, &cm.type)) {
                if (err) *err = "mapping: unknown column type " + ct->text;
                return false;
            }
            const JVal* nn = c.Find("nullable");
            if (nn != nullptr && nn->type == JVal::T::kBool) cm.nullable = nn->boolean;
            if (defaults != nullptr) {
                const JVal* d = defaults->Find(cm.dst);
                if (d != nullptr && d->type == JVal::T::kStr) {
                    cm.has_default = true;
                    cm.def = d->text;
                }
            }
            tm.columns.push_back(std::move(cm));
        }
        if (tm.primary_key.empty() && !tm.columns.empty()) tm.primary_key = tm.columns[0].dst;
        out->tables.push_back(std::move(tm));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 迁移与校验
// ---------------------------------------------------------------------------
struct MigrateParams {
    std::string source;
    std::string mapping;
    std::string target;
    std::string report;
    bool dry_run = false;
    uint64_t limit = 0;      ///< 每表最大迁移行数（0 = 不限）
    uint64_t batch_rows = 512;
    uint64_t target_size_mb = 256;  ///< 新建目标映像大小（单测可缩小加速）
};
struct TableReport {
    std::string table;
    uint64_t source_rows = 0;
    uint64_t target_rows = 0;
    uint64_t key_checksum = 0;
    uint32_t samples = 0;
    bool sample_ok = true;
};
struct MigrationReport {
    bool dry_run = false;
    bool passed = false;
    uint64_t elapsed_ms = 0;
    int64_t committed_seq = -1;
    int64_t checkpoint_seq = -1;
    std::vector<TableReport> tables;
    std::string note;
};

inline bool SameValue(const ColumnValue& a, const ColumnValue& b) {
    if (a.is_null() || b.is_null()) return a.is_null() && b.is_null();
    const DataType t = a.type();
    if (t == DataType::kFloat32 || t == DataType::kFloat64)
        return a.as_real() == b.as_real();
    if (t == DataType::kUtf8Var || t == DataType::kBytesVar)
        return a.as_bytes() == b.as_bytes();
    return a.as_int() == b.as_int();
}

inline Err Fail(std::string* err, Err code, std::string msg) {
    if (err != nullptr) *err = std::move(msg);
    return code;
}

inline Err Run(const MigrateParams& p, MigrationReport* rep, std::string* err) {
    if (err != nullptr) err->clear();
    const auto t0 = std::chrono::steady_clock::now();
    rep->dry_run = p.dry_run;
    rep->note = "源映像格式：自描述 fixture（真实 PSRtdb 二进制格式不可得，适配器接口可替换）";

    std::string mtext;
    if (!ReadFile(p.mapping, &mtext, err)) return Err::kIoError;
    Mapping mapping;
    if (!ParseMapping(mtext, &mapping, err)) return Err::kInvalidArgument;

    FixtureAdapter adapter(p.source);
    std::vector<SrcTable> source_tables;
    if (!adapter.Tables(&source_tables, err)) return Err::kInvalidArgument;

    // 目标表映射按表名索引；未知源表/未知列一律明确报错，不静默跳过。
    std::map<std::string, const SrcTable*> by_name;
    for (const SrcTable& t : source_tables) by_name[t.name] = &t;
    for (const TableMap& tm : mapping.tables) {
        const auto it = by_name.find(tm.src);
        if (it == by_name.end())
            return Fail(err, Err::kNotFound, "mapping: source table not in image: " + tm.src);
        const SrcTable& st = *it->second;
        std::map<std::string, size_t> src_index;
        for (size_t i = 0; i < st.columns.size(); ++i) src_index[st.columns[i].name] = i;
        for (const ColumnMap& cm : tm.columns) {
            if (src_index.find(cm.src) == src_index.end())
                return Fail(err, Err::kInvalidArgument,
                            "mapping: unknown source column " + tm.src + "." + cm.src);
        }
        TableReport tr;
        tr.table = tm.dst;
        tr.source_rows = st.rows.size();
        if (p.limit != 0 && tr.source_rows > p.limit) tr.source_rows = p.limit;
        rep->tables.push_back(tr);
    }
    if (p.dry_run) {
        rep->passed = true;
        rep->elapsed_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        return Err::kOk;
    }

    // 目标映像必须不存在：拒绝静默覆盖既有数据映像。
    {
        std::ifstream probe(p.target, std::ios::binary);
        if (probe)
            return Fail(err, Err::kAlreadyExists,
                        "target image already exists, remove it first: " + p.target);
    }
    Options opts;
    opts.instance = "migrate_from_psrtdb";
    opts.path = p.target;
    opts.initial_size_bytes = (p.target_size_mb == 0 ? 256 : p.target_size_mb) << 20;
    opts.role = Role::Standalone;
    opts.wal_ring_bytes = 1ull << 20;
    opts.wal_fsync = FsyncPolicy::PerSecond;
    auto opened = Engine::Open(opts);
    if (!opened.IsOk()) return Fail(err, opened.Error(), "open target: " + std::string(ErrorMessage(opened.Error())));
    std::unique_ptr<Engine> eng = opened.TakeValue();

    for (size_t ti = 0; ti < mapping.tables.size(); ++ti) {
        const TableMap& tm = mapping.tables[ti];
        TableReport& tr = rep->tables[ti];
        const SrcTable& st = *by_name[tm.src];
        std::map<std::string, size_t> src_index;
        for (size_t i = 0; i < st.columns.size(); ++i) src_index[st.columns[i].name] = i;

        TableDef def;
        def.name = tm.dst;
        def.max_rows_hint = static_cast<size_t>(tr.source_rows + 64);
        for (const ColumnMap& cm : tm.columns) {
            ColumnDef cd;
            cd.name = cm.dst;
            cd.type = cm.type;
            cd.nullable = cm.nullable;
            def.columns.push_back(std::move(cd));
        }
        auto created = eng->CreateTable(def);
        if (!created.IsOk())
            return Fail(err, created.Error(), "create table " + tm.dst + ": " +
                                                  std::string(ErrorMessage(created.Error())));
        const TableHandle table = created.Value();

        std::vector<std::vector<ColumnValue>> converted;
        converted.reserve(static_cast<size_t>(tr.source_rows));
        for (uint64_t r = 0; r < tr.source_rows; ++r) {
            const std::vector<ColumnValue>& srow = st.rows[static_cast<size_t>(r)];
            std::vector<ColumnValue> row;
            row.reserve(tm.columns.size());
            for (const ColumnMap& cm : tm.columns) {
                const size_t si = src_index[cm.src];
                ColumnValue v = srow[si];
                if (v.is_null() && cm.has_default) {
                    JVal j;
                    j.type = JVal::T::kStr;
                    j.text = cm.def;
                    if (!ToValue(j, cm.type, &v, err)) return Err::kInvalidArgument;
                }
                if (v.is_null() && !cm.nullable)
                    return Fail(err, Err::kInvalidArgument,
                                "NOT NULL column has NULL value: " + cm.dst);
                // 目标类型与源类型不同时按目标类型重新标注（整数类/浮点/变长分开）。
                if (!v.is_null()) {
                    if (cm.type == DataType::kFloat32 || cm.type == DataType::kFloat64) {
                        const double d = (v.type() == DataType::kFloat32 ||
                                          v.type() == DataType::kFloat64) ? v.as_real()
                                         : static_cast<double>(v.as_int());
                        v = ColumnValue::Real(d);
                    } else if (cm.type == DataType::kUtf8Var || cm.type == DataType::kBytesVar) {
                        v = cm.type == DataType::kBytesVar ? ColumnValue::Bytes(v.as_bytes())
                                                           : ColumnValue::Text(v.as_bytes());
                    } else {
                        v = ColumnValue::IntOf(cm.type, v.as_int());
                    }
                }
                row.push_back(std::move(v));
            }
            converted.push_back(std::move(row));
        }

        const uint64_t batch = p.batch_rows == 0 ? 512 : p.batch_rows;
        for (uint64_t off = 0; off < converted.size(); off += batch) {
            std::vector<Engine::BatchOp> ops;
            const uint64_t end = std::min<uint64_t>(converted.size(), off + batch);
            ops.reserve(static_cast<size_t>(end - off));
            for (uint64_t i = off; i < end; ++i) {
                Engine::BatchOp op;
                op.type = Engine::BatchOp::Type::kInsert;
                op.table = table;
                op.row = converted[static_cast<size_t>(i)];
                ops.push_back(std::move(op));
            }
            auto done = eng->ExecuteBatch(ops);
            if (!done.IsOk())
                return Fail(err, done.Error(), "batch write " + tm.dst + ": " +
                                                   std::string(ErrorMessage(done.Error())));
        }

        // 校验：全表重读 → 行数 / 主键集合校验和 / 抽样逐值比对。
        auto scanned = eng->ScanAll(table);
        if (!scanned.IsOk())
            return Fail(err, scanned.Error(), "verify scan " + tm.dst + ": " +
                                                  std::string(ErrorMessage(scanned.Error())));
        const std::vector<std::vector<ColumnValue>>& rows = scanned.Value();
        tr.target_rows = rows.size();
        std::vector<int64_t> keys;
        keys.reserve(rows.size());
        for (const auto& row : rows) if (!row.empty()) keys.push_back(row[0].as_int());
        std::sort(keys.begin(), keys.end());
        uint64_t sum = 1469598103934665603ull;
        for (const int64_t k : keys) {
            sum ^= static_cast<uint64_t>(k);
            sum *= 1099511628211ull;
        }
        tr.key_checksum = sum;
        tr.samples = static_cast<uint32_t>(std::min<size_t>(5, rows.size()));
        for (size_t i = 0; i < tr.samples; ++i) {
            if (rows[i].size() != converted[i].size()) { tr.sample_ok = false; break; }
            for (size_t c = 0; c < rows[i].size(); ++c) {
                if (!SameValue(rows[i][c], converted[i][c])) { tr.sample_ok = false; break; }
            }
            if (!tr.sample_ok) break;
        }
        if (tr.target_rows != tr.source_rows || !tr.sample_ok)
            return Fail(err, Err::kCorruption, "verification failed for table " + tm.dst);
    }

    auto snap = eng->CreateSnapshot();
    if (!snap.IsOk())
        return Fail(err, snap.Error(), "snapshot: " + std::string(ErrorMessage(snap.Error())));
    rep->committed_seq = eng->CommittedSeq();
    rep->checkpoint_seq = eng->CheckpointSeq();
    rep->passed = true;
    rep->elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    return Err::kOk;
}

inline std::string ReportJson(const MigrationReport& r) {
    std::string s = "{";
    s += "\"passed\":" + std::string(r.passed ? "true" : "false");
    s += ",\"dry_run\":" + std::string(r.dry_run ? "true" : "false");
    s += ",\"elapsed_ms\":" + std::to_string(r.elapsed_ms);
    s += ",\"committed_seq\":" + std::to_string(r.committed_seq);
    s += ",\"checkpoint_seq\":" + std::to_string(r.checkpoint_seq);
    s += ",\"note\":" + Quote(r.note);
    s += ",\"tables\":[";
    for (size_t i = 0; i < r.tables.size(); ++i) {
        const TableReport& t = r.tables[i];
        if (i != 0) s += ",";
        s += "{\"table\":" + Quote(t.table);
        s += ",\"source_rows\":" + std::to_string(t.source_rows);
        s += ",\"target_rows\":" + std::to_string(t.target_rows);
        s += ",\"key_checksum\":" + std::to_string(t.key_checksum);
        s += ",\"samples\":" + std::to_string(t.samples);
        s += ",\"sample_ok\":" + std::string(t.sample_ok ? "true" : "false") + "}";
    }
    s += "]}";
    return s;
}

inline bool MakeFixture(const std::string& path, std::string* err) {
    const char* kSample =
        "{\n"
        "  \"format\": \"rtdb_fixture_v1\",\n"
        "  \"tables\": [\n"
        "    {\n"
        "      \"name\": \"meters\",\n"
        "      \"columns\": [\n"
        "        {\"name\": \"id\", \"type\": \"int64\", \"nullable\": false},\n"
        "        {\"name\": \"val\", \"type\": \"float64\", \"nullable\": true},\n"
        "        {\"name\": \"name\", \"type\": \"utf8\", \"nullable\": true}\n"
        "      ],\n"
        "      \"rows\": [\n"
        "        [1, 1.5, \"alpha\"],\n"
        "        [2, null, \"beta\"],\n"
        "        [3, 3.5, null]\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    return WriteFile(path, kSample, err);
}

}  // namespace migrate
}  // namespace rtdb

#endif  // RTDB_TOOLS_MIGRATE_CORE_HPP_
