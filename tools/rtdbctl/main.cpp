/// \file
/// \brief M4 rtdbctl 运维 CLI（docs/16 D9）：W1 交付 status/authz me，
/// W2 追加 ddl create/describe/drop，W3 追加数据面 import/export/subscribe。
/// 连接/鉴权参数与 bench_net 一致（口令摘要 = SHA-256(salt || password)）。
///
/// 用法：
///   rtdbctl status [--json] [连接参数]
///   rtdbctl authz me [--json] [连接参数]
///   rtdbctl authz show [--json] [连接参数]
///   rtdbctl authz check <table> [r|w] [--json] [连接参数]
///   rtdbctl ddl describe <table> [--json] [连接参数]
///   rtdbctl ddl drop <table> [连接参数]
///   rtdbctl ddl create <table> --col <name:type[:nn|:null]>... [--row-cap N] [连接参数]
///   rtdbctl import <table> --file <path.csv|path.json> [--batch N] [--json] [连接参数]
///   rtdbctl export <table> [--file <path>] [--json] [连接参数]
///   rtdbctl subscribe <table> [--seconds N] [连接参数]
///
/// 数据面约定（docs/16 D9）：
///  - import：CSV 首行为表头（列名）或 JSON 行数组；按批预算分片，逐批
///    ExecuteBatch 原子写；总行数不超批预算时整文件一次原子批（失败零副作用）。
///    CSV 以空字段表示 NULL、`""` 表示空串，JSON 以 null 表示 NULL；
///  - export：分页 ScanPaged 扫描（不物化全表），CSV 带表头、JSON 为行数组；
///  - subscribe：订阅打印，seq 有序，Ctrl+C/SIGTERM 优雅退出，输出格式与
///    examples/remote_demo 一致便于双端比对。
///
/// 退出码：0 = 成功 / authz check 有权限；1 = 请求/连接/IO 失败 / authz check
/// 无权限（DENY）；2 = 用法或输入数据错误。

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform/console.hpp"
#include "rtdb/err.hpp"
#include "rtdb/rtdb_client.hpp"
#include "rtdb/table_def.hpp"

namespace {

struct Args {
    std::string host = "127.0.0.1";
    std::string port_str;
    uint16_t port = 0;
    std::string user;
    std::string password;
    std::string salt;
    bool json = false;
    uint32_t row_cap = 0;
    uint32_t seconds = 0;     // subscribe 运行秒数（0 = 直到 Ctrl+C）
    uint32_t batch_rows = 0;  // import 批预算覆盖（0 = 用默认 kImportBatchRows）
    std::string file;         // import 输入 / export 输出路径（空 = stdout）
    std::vector<std::string> pos;
    std::vector<std::string> cols;
};

void Usage() {
    std::fprintf(stderr,
                 "用法：rtdbctl <command> [选项]\n"
                 "  status [--json]\n"
                 "  authz me [--json]\n"
                 "  authz show [--json]\n"
                 "  authz check <table> [r|w] [--json]\n"
                 "  ddl describe <table> [--json]\n"
                 "  ddl drop <table>\n"
                 "  ddl create <table> --col <name:type[:nn|:null]>... [--row-cap N]\n"
                 "  import <table> --file <path.csv|path.json> [--batch N] [--json]\n"
                 "  export <table> [--file <path>] [--json]\n"
                 "  subscribe <table> [--seconds N]\n"
                 "连接：--host H --port P --user U --password PW --salt S\n"
                 "type：int8/int16/int32/int64/bool/float32/float64/utf8/bytes/ts\n"
                 "首个 --col 为主键（恒 NOT NULL）；其余列默认 nullable。\n"
                 "import/export：文件按扩展名或首个非空字符（'['）识别 JSON，\n"
                 "  否则按 CSV；CSV 空字段=NULL、\"\"=空串，export 变长值一律加引号。\n");
}

bool ParsePort(const std::string& s, uint16_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v > 65535) return false;
    *out = static_cast<uint16_t>(v);
    return true;
}

bool ParseU32(const std::string& s, uint32_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v > 0xFFFFFFFFul) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool ParseDataType(const std::string& s, rtdb::DataType* out) {
    const std::string t = Lower(s);
    if (t == "int8" || t == "i8") {
        *out = rtdb::DataType::kInt8;
    } else if (t == "int16" || t == "i16") {
        *out = rtdb::DataType::kInt16;
    } else if (t == "int32" || t == "i32" || t == "int") {
        *out = rtdb::DataType::kInt32;
    } else if (t == "int64" || t == "i64" || t == "long") {
        *out = rtdb::DataType::kInt64;
    } else if (t == "bool" || t == "boolean") {
        *out = rtdb::DataType::kBool;
    } else if (t == "float32" || t == "f32") {
        *out = rtdb::DataType::kFloat32;
    } else if (t == "float64" || t == "f64" || t == "double") {
        *out = rtdb::DataType::kFloat64;
    } else if (t == "utf8" || t == "string" || t == "text" || t == "var") {
        *out = rtdb::DataType::kUtf8Var;
    } else if (t == "bytes" || t == "blob") {
        *out = rtdb::DataType::kBytesVar;
    } else if (t == "ts" || t == "timestamp" || t == "unix_ts_ns") {
        *out = rtdb::DataType::kUnixTsNs;
    } else {
        return false;
    }
    return true;
}

const char* DataTypeName(rtdb::DataType t) {
    switch (t) {
        case rtdb::DataType::kInt8:
            return "int8";
        case rtdb::DataType::kInt16:
            return "int16";
        case rtdb::DataType::kInt32:
            return "int32";
        case rtdb::DataType::kInt64:
            return "int64";
        case rtdb::DataType::kFloat32:
            return "float32";
        case rtdb::DataType::kFloat64:
            return "float64";
        case rtdb::DataType::kBool:
            return "bool";
        case rtdb::DataType::kUtf8Var:
            return "utf8";
        case rtdb::DataType::kBytesVar:
            return "bytes";
        case rtdb::DataType::kUnixTsNs:
            return "ts";
    }
    return "unknown";
}

/// `<name:type[:nn|:null]>` → ColumnDef（首个列由调用方强制 NOT NULL）。
bool ParseColumnSpec(const std::string& spec, rtdb::ColumnDef* out, std::string* why) {
    const size_t c1 = spec.find(':');
    if (c1 == std::string::npos || c1 == 0) {
        *why = "--col 需 name:type[:nn|:null]";
        return false;
    }
    const size_t c2 = spec.find(':', c1 + 1);
    const std::string name = spec.substr(0, c1);
    const std::string type =
        spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
    const std::string flag = c2 == std::string::npos ? "" : Lower(spec.substr(c2 + 1));
    out->name = name;
    out->nullable = true;
    if (!ParseDataType(type, &out->type)) {
        *why = "未知列类型：" + type;
        return false;
    }
    if (flag == "nn" || flag == "notnull" || flag == "not_null") {
        out->nullable = false;
    } else if (flag.empty() || flag == "null" || flag == "nullable") {
        out->nullable = true;
    } else {
        *why = "未知列修饰（仅 nn/null）：" + flag;
        return false;
    }
    return true;
}

/// 极简 JSON 字符串转义（指标名/表名/列名为标识符，只需引号/反斜杠）。
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

/// 表名白名单匹配（规则与 src/server/authz.cpp 的 TablePatternMatches 一致）：
/// `*` 全匹配、`前缀*` 前缀匹配、其余精确匹配；空模板不匹配。
bool TableMatches(const std::string& pattern, const std::string& table) {
    if (pattern.empty()) return false;
    if (pattern == "*") return true;
    if (pattern.back() == '*') {
        const std::string prefix = pattern.substr(0, pattern.size() - 1);
        if (prefix.empty()) return true;
        return table.size() >= prefix.size() &&
               table.compare(0, prefix.size(), prefix) == 0;
    }
    return pattern == table;
}

bool ListAllows(const std::vector<std::string>& patterns, const std::string& table) {
    for (const std::string& p : patterns) {
        if (TableMatches(p, table)) return true;
    }
    return false;
}

/// authz show：打印全部用户的授权摘要（admin/ddl 标志 + r/w 白名单）。
int PrintAuthzShow(rtdb::Client& cli, bool json) {
    auto res = cli.GetAuthzShow();
    if (!res.IsOk()) {
        std::fprintf(stderr, "读取用户权限摘要失败：%s\n",
                     rtdb::ErrorMessage(res.Error()));
        return 1;
    }
    const std::vector<rtdb::AuthzInfo>& list = res.Value();
    if (json) {
        std::printf("[");
        for (size_t i = 0; i < list.size(); ++i) {
            const rtdb::AuthzInfo& ai = list[i];
            std::printf("%s{\"user\":\"%s\",\"admin\":%s,\"ddl\":%s,"
                        "\"legacy\":%s,\"read\":[",
                        i == 0 ? "" : ",", JsonEscape(ai.user).c_str(),
                        ai.admin ? "true" : "false", ai.ddl ? "true" : "false",
                        ai.legacy ? "true" : "false");
            for (size_t j = 0; j < ai.read_tables.size(); ++j) {
                std::printf("%s\"%s\"", j == 0 ? "" : ",",
                            JsonEscape(ai.read_tables[j]).c_str());
            }
            std::printf("],\"write\":[");
            for (size_t j = 0; j < ai.write_tables.size(); ++j) {
                std::printf("%s\"%s\"", j == 0 ? "" : ",",
                            JsonEscape(ai.write_tables[j]).c_str());
            }
            std::printf("]}");
        }
        std::printf("]\n");
    } else {
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0) std::printf("\n");
            const rtdb::AuthzInfo& ai = list[i];
            std::printf("user   = %s\n", ai.user.c_str());
            std::printf("admin  = %s\n", ai.admin ? "true" : "false");
            std::printf("ddl    = %s\n", ai.ddl ? "true" : "false");
            std::printf("legacy = %s%s\n", ai.legacy ? "true" : "false",
                        ai.legacy ? "  (M3 兼容用户，实际按 admin 放行)" : "");
            std::printf("read   =");
            for (const std::string& t : ai.read_tables) std::printf(" %s", t.c_str());
            std::printf("\nwrite  =");
            for (const std::string& t : ai.write_tables) std::printf(" %s", t.c_str());
            std::printf("\n");
        }
    }
    return 0;
}

/// authz check：按表判断当前用户是否具备 r/w 权限；admin 隐含全部。
/// 退出码 0 = PASS（有权限）、1 = DENY（无权限）或请求失败、2 = 用法错误。
int AuthzCheck(rtdb::Client& cli, const std::string& table,
               const std::string& mode, bool json) {
    auto info = cli.GetAuthzInfo();
    if (!info.IsOk()) {
        std::fprintf(stderr, "读取权限摘要失败：%s\n",
                     rtdb::ErrorMessage(info.Error()));
        return 1;
    }
    const rtdb::AuthzInfo& ai = info.Value();
    // admin 隐含全部表/DDL 权限（docs/16 D1）；legacy（M3 兼容空规约）
    // 在服务端同样按 admin 放行，但白名单为空，须显式视作全权限，
    // 否则 `rtdbctl authz check` 会对 M3 默认用户误报 DENY。
    // 其余按 read/write 白名单逐项匹配，规则同服务端 TablePatternMatches。
    bool allowed = ai.admin || ai.legacy;
    if (!allowed) {
        const std::vector<std::string>& list =
            (mode == "w") ? ai.write_tables : ai.read_tables;
        allowed = ListAllows(list, table);
    }
    if (json) {
        std::printf("{\"result\":\"%s\",\"user\":\"%s\",\"table\":\"%s\","
                    "\"mode\":\"%s\"}\n",
                    allowed ? "PASS" : "DENY", JsonEscape(ai.user).c_str(),
                    JsonEscape(table).c_str(), mode.c_str());
    } else {
        std::printf("%s user=%s table=%s mode=%s\n",
                    allowed ? "PASS" : "DENY", ai.user.c_str(), table.c_str(),
                    mode.c_str());
    }
    return allowed ? 0 : 1;
}

int PrintStats(rtdb::Client& cli, bool json) {
    auto st = cli.GetServerStats();
    if (!st.IsOk()) {
        std::fprintf(stderr, "读取 STATS 失败：%s\n", rtdb::ErrorMessage(st.Error()));
        return 1;
    }
    const auto& vals = st.Value().values;
    if (json) {
        std::printf("{");
        bool first = true;
        for (const auto& kv : vals) {
            std::printf("%s\"%s\":%llu", first ? "" : ",",
                        JsonEscape(kv.first).c_str(),
                        static_cast<unsigned long long>(kv.second));
            first = false;
        }
        std::printf("}\n");
    } else {
        std::printf("%-20s %s\n", "指标", "值");
        for (const auto& kv : vals) {
            std::printf("%-20s %llu\n", kv.first.c_str(),
                        static_cast<unsigned long long>(kv.second));
        }
    }
    return 0;
}

int PrintAuthz(rtdb::Client& cli, bool json) {
    auto info = cli.GetAuthzInfo();
    if (!info.IsOk()) {
        std::fprintf(stderr, "读取权限摘要失败：%s\n", rtdb::ErrorMessage(info.Error()));
        return 1;
    }
    const rtdb::AuthzInfo& ai = info.Value();
    if (json) {
        std::printf("{\"user\":\"%s\",\"admin\":%s,\"ddl\":%s,\"legacy\":%s,\"read\":[",
                    JsonEscape(ai.user).c_str(), ai.admin ? "true" : "false",
                    ai.ddl ? "true" : "false", ai.legacy ? "true" : "false");
        for (size_t i = 0; i < ai.read_tables.size(); ++i) {
            std::printf("%s\"%s\"", i == 0 ? "" : ",",
                        JsonEscape(ai.read_tables[i]).c_str());
        }
        std::printf("],\"write\":[");
        for (size_t i = 0; i < ai.write_tables.size(); ++i) {
            std::printf("%s\"%s\"", i == 0 ? "" : ",",
                        JsonEscape(ai.write_tables[i]).c_str());
        }
        std::printf("]}\n");
    } else {
        std::printf("user   = %s\n", ai.user.c_str());
        std::printf("admin  = %s\n", ai.admin ? "true" : "false");
        std::printf("ddl    = %s\n", ai.ddl ? "true" : "false");
        std::printf("legacy = %s\n", ai.legacy ? "true" : "false");
        std::printf("read   =");
        for (const std::string& t : ai.read_tables) std::printf(" %s", t.c_str());
        std::printf("\nwrite  =");
        for (const std::string& t : ai.write_tables) std::printf(" %s", t.c_str());
        std::printf("\n");
    }
    return 0;
}

int DdlDescribe(rtdb::Client& cli, const std::string& table, bool json) {
    auto s = cli.DescribeTable(table);
    if (!s.IsOk()) {
        std::fprintf(stderr, "describe 失败：%s\n", rtdb::ErrorMessage(s.Error()));
        return 1;
    }
    const rtdb::TableSchema& sc = s.Value();
    if (json) {
        std::printf("{\"table\":\"%s\",\"table_id\":%lld,\"layout_version\":%u,\"columns\":[",
                    JsonEscape(sc.name).c_str(), static_cast<long long>(sc.table_id),
                    static_cast<unsigned>(sc.layout_version));
        for (size_t i = 0; i < sc.col_names.size(); ++i) {
            std::printf("%s{\"name\":\"%s\",\"type\":\"%s\"}", i == 0 ? "" : ",",
                        JsonEscape(sc.col_names[i]).c_str(),
                        i < sc.col_types.size() ? DataTypeName(sc.col_types[i]) : "?");
        }
        std::printf("]}\n");
    } else {
        std::printf("table    = %s\n", sc.name.c_str());
        std::printf("table_id = %lld\n", static_cast<long long>(sc.table_id));
        std::printf("layout   = %u\n", static_cast<unsigned>(sc.layout_version));
        std::printf("%-20s %s\n", "列", "类型");
        for (size_t i = 0; i < sc.col_names.size(); ++i) {
            std::printf("%-20s %s\n", sc.col_names[i].c_str(),
                        i < sc.col_types.size() ? DataTypeName(sc.col_types[i]) : "?");
        }
    }
    return 0;
}

int DdlCreate(rtdb::Client& cli, const std::string& table, const Args& args) {
    if (args.cols.empty()) {
        std::fprintf(stderr, "ddl create 需要至少一个 --col\n");
        return 2;
    }
    rtdb::TableDef def;
    def.name = table;
    def.row_cap = args.row_cap;
    for (size_t i = 0; i < args.cols.size(); ++i) {
        rtdb::ColumnDef c;
        std::string why;
        if (!ParseColumnSpec(args.cols[i], &c, &why)) {
            std::fprintf(stderr, "非法 --col（%s）：%s\n", args.cols[i].c_str(), why.c_str());
            return 2;
        }
        if (i == 0) c.nullable = false;  // 主键列恒 NOT NULL
        def.columns.push_back(std::move(c));
    }
    auto r = cli.CreateTable(def);
    if (!r.IsOk()) {
        std::fprintf(stderr, "建表失败：%s\n", rtdb::ErrorMessage(r.Error()));
        return 1;
    }
    const rtdb::CreateTableInfo& info = r.Value();
    if (args.json) {
        std::printf("{\"table_id\":%lld,\"visible_columns\":%u,\"layout_version\":%u}\n",
                    static_cast<long long>(info.table_id),
                    static_cast<unsigned>(info.visible_columns),
                    static_cast<unsigned>(info.layout_version));
    } else {
        std::printf("创建成功：table_id=%lld visible_columns=%u layout_version=%u\n",
                    static_cast<long long>(info.table_id),
                    static_cast<unsigned>(info.visible_columns),
                    static_cast<unsigned>(info.layout_version));
    }
    return 0;
}

// ===================== W3 数据面（docs/16 D9）=====================

/// import 单批行数预算：单次 EXEC_BATCH 不超过该行数，避免超帧上限；
/// export 每页行数（ScanPaged 分页，不物化全表）。
constexpr uint32_t kImportBatchRows = 256;
constexpr uint32_t kExportPageRows = 256;

/// Ctrl+C / SIGTERM 只置停止标志，由 subscribe 循环收尾（优雅退出）。
std::atomic<bool> g_stop{false};
extern "C" void OnSignal(int) { g_stop.store(true); }

/// 整文件读入内存（binary，避免 Windows 文本模式改动换行）。
bool ReadAllFile(const std::string& path, std::string* out, std::string* why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *why = "无法打开文件：" + path;
        return false;
    }
    std::string data;
    char buf[4096];
    while (in.read(buf, static_cast<std::streamsize>(sizeof(buf))) || in.gcount() > 0) {
        data.append(buf, static_cast<size_t>(in.gcount()));
    }
    if (in.bad()) {
        *why = "读取文件失败：" + path;
        return false;
    }
    *out = std::move(data);
    return true;
}

bool ParseI64(const std::string& s, int64_t* out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || errno == ERANGE) return false;
    *out = static_cast<int64_t>(v);
    return true;
}

bool ParseF64(const std::string& s, double* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    *out = v;
    return true;
}

/// %.17g 保证 double 文本往返；NaN/Inf 会输出 nan/inf（非严格 JSON），
/// 属 M4 范围外的已知边界，见交付报告。
std::string RealToStr(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
}

int FindColumn(const rtdb::TableSchema& schema, const std::string& name) {
    for (size_t i = 0; i < schema.col_names.size(); ++i) {
        if (schema.col_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

/// 输入格式判定：扩展名 .json 优先，其次以首个非空字符 '[' 识别 JSON 行数组。
bool IsJsonInput(const std::string& path, const std::string& text) {
    const size_t dot = path.rfind('.');
    if (dot != std::string::npos) {
        const std::string ext = Lower(path.substr(dot + 1));
        if (ext == "json") return true;
    }
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '[';
    }
    return false;
}

/// CSV 字段文本 → ColumnValue（按目标列声明类型转换；与 c_abi.cpp 的
/// FromCValue 同构：浮点用 Real、变长用 Text/Bytes、其余用 IntOf/Bool）。
bool TextToValue(const std::string& s, rtdb::DataType t, rtdb::ColumnValue* out,
                 std::string* why) {
    switch (t) {
        case rtdb::DataType::kUtf8Var:
            *out = rtdb::ColumnValue::Text(s);
            return true;
        case rtdb::DataType::kBytesVar:
            *out = rtdb::ColumnValue::Bytes(s);
            return true;
        case rtdb::DataType::kBool: {
            const std::string l = Lower(s);
            if (l == "true" || l == "1") {
                *out = rtdb::ColumnValue::Bool(true);
                return true;
            }
            if (l == "false" || l == "0") {
                *out = rtdb::ColumnValue::Bool(false);
                return true;
            }
            *why = "布尔列非法值：" + s;
            return false;
        }
        case rtdb::DataType::kFloat32:
        case rtdb::DataType::kFloat64: {
            double d = 0.0;
            if (!ParseF64(s, &d)) {
                *why = "浮点列非法值：" + s;
                return false;
            }
            *out = rtdb::ColumnValue::Real(d);
            return true;
        }
        default: {
            int64_t i = 0;
            if (!ParseI64(s, &i)) {
                *why = "整数列非法值：" + s;
                return false;
            }
            *out = rtdb::ColumnValue::IntOf(t, i);
            return true;
        }
    }
}

struct CsvField {
    std::string text;
    bool quoted = false;  // 是否曾被双引号包裹（区分 NULL 与空串）
};

/// 整段文本 → CSV 记录：支持双引号、"" 转义、引号内逗号与换行；
/// 空字段（未加引号）= NULL，`""` = 空串。引号未闭合返回 false。
bool ParseCsvRecords(const std::string& text,
                     std::vector<std::vector<CsvField>>* out) {
    std::vector<CsvField> fields;
    CsvField cur;
    bool in_quotes = false;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const char c = text[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < n && text[i + 1] == '"') {
                    cur.text.push_back('"');
                    i += 2;
                } else {
                    in_quotes = false;
                    ++i;
                }
            } else {
                cur.text.push_back(c);  // 引号内逗号/换行均为字段内容
                ++i;
            }
            continue;
        }
        if (c == '"' && cur.text.empty()) {
            in_quotes = true;
            cur.quoted = true;
            ++i;
        } else if (c == ',') {
            fields.push_back(std::move(cur));
            cur = CsvField();
            ++i;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < n && text[i + 1] == '\n') ++i;
            ++i;
            const bool empty_record = fields.empty() && cur.text.empty() && !cur.quoted;
            if (!empty_record) {
                fields.push_back(std::move(cur));
                out->push_back(std::move(fields));
            }
            fields.clear();
            cur = CsvField();
        } else {
            cur.text.push_back(c);
            ++i;
        }
    }
    if (in_quotes) return false;
    if (!cur.text.empty() || cur.quoted || !fields.empty()) {
        fields.push_back(std::move(cur));
        out->push_back(std::move(fields));
    }
    return true;
}

/// CSV 文本 → 按 schema 可见列序排列的整行值（首行表头 = 列名）。
/// 表头未知列/重复列、数据行列数不符、类型非法一律在零写入阶段拒绝。
bool ParseCsvRows(const std::string& text, const rtdb::TableSchema& schema,
                  std::vector<std::vector<rtdb::ColumnValue>>* rows, std::string* why) {
    if (schema.col_names.empty() || schema.col_types.size() != schema.col_names.size()) {
        *why = "表 schema 列名/类型数量不一致";
        return false;
    }
    std::vector<std::vector<CsvField>> recs;
    if (!ParseCsvRecords(text, &recs)) {
        *why = "CSV 双引号未闭合";
        return false;
    }
    if (recs.empty()) {
        *why = "CSV 缺少表头行";
        return false;
    }
    const std::vector<CsvField>& head = recs[0];
    std::vector<int> col_index(head.size(), -1);
    for (size_t j = 0; j < head.size(); ++j) {
        const int ci = FindColumn(schema, head[j].text);
        if (ci < 0) {
            *why = "CSV 表头含未知列：" + head[j].text;
            return false;
        }
        col_index[j] = ci;
    }
    for (size_t j = 0; j < col_index.size(); ++j) {
        for (size_t k = j + 1; k < col_index.size(); ++k) {
            if (col_index[j] == col_index[k]) {
                *why = "CSV 表头列名重复：" + head[j].text;
                return false;
            }
        }
    }
    rows->clear();
    rows->reserve(recs.size() - 1);
    for (size_t r = 1; r < recs.size(); ++r) {
        const std::vector<CsvField>& rec = recs[r];
        if (rec.size() != head.size()) {
            *why = "CSV 第 " + std::to_string(r + 1) + " 行列数与表头不符";
            return false;
        }
        // 未出现在表头的列留 NULL，由服务端按 nullable 约束裁决。
        std::vector<rtdb::ColumnValue> row(
            schema.col_names.size(), rtdb::ColumnValue::Null());
        for (size_t j = 0; j < rec.size(); ++j) {
            const CsvField& f = rec[j];
            if (f.text.empty() && !f.quoted) continue;  // 空字段 = NULL
            const size_t ci = static_cast<size_t>(col_index[j]);
            std::string err;
            rtdb::ColumnValue v;
            if (!TextToValue(f.text, schema.col_types[ci], &v, &err)) {
                *why = "CSV 第 " + std::to_string(r + 1) + " 行：" + err;
                return false;
            }
            row[ci] = std::move(v);
        }
        rows->push_back(std::move(row));
    }
    return true;
}

/// 极简 JSON 值（仅覆盖 import 行数组子集）；全程 bool 返回、零异常穿越。
struct JVal {
    enum class K : uint8_t { kNull, kBool, kNum, kStr, kArr, kObj };
    K k = K::kNull;
    bool b = false;
    bool num_is_int = false;
    int64_t inum = 0;
    double num = 0.0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
};

/// 递归下降 JSON 解析器：对象/数组/字符串（含 \uXXXX 与代理对）/数字/
/// true/false/null。不引入第三方库；非法输入返回 false 而非抛异常。
class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool Parse(JVal* out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        return pos_ == s_.size();
    }

private:
    void SkipWs() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool Literal(const char* word) {
        const size_t len = std::strlen(word);
        if (s_.compare(pos_, len, word) != 0) return false;
        pos_ += len;
        return true;
    }

    bool ParseValue(JVal* v) {
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') return ParseObject(v);
        if (c == '[') return ParseArray(v);
        if (c == '"') {
            v->k = JVal::K::kStr;
            return ParseString(&v->str);
        }
        if (c == 'n') {
            if (!Literal("null")) return false;
            v->k = JVal::K::kNull;
            return true;
        }
        if (c == 't') {
            if (!Literal("true")) return false;
            v->k = JVal::K::kBool;
            v->b = true;
            return true;
        }
        if (c == 'f') {
            if (!Literal("false")) return false;
            v->k = JVal::K::kBool;
            v->b = false;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(v);
        return false;
    }

    bool ParseString(std::string* out) {
        ++pos_;  // 跳过起始双引号
        out->clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
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
                        if (!ParseHex4(&cp)) return false;
                        if (cp >= 0xD800u && cp <= 0xDBFFu) {
                            // 高位代理须紧跟 \uXXXX 低位代理
                            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' ||
                                s_[pos_ + 1] != 'u') {
                                return false;
                            }
                            pos_ += 2;
                            uint32_t lo = 0;
                            if (!ParseHex4(&lo) || lo < 0xDC00u || lo > 0xDFFFu) return false;
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                            return false;  // 孤立低位代理
                        }
                        AppendUtf8(cp, out);
                        break;
                    }
                    default: return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return false;  // 控制字符必须转义
            } else {
                out->push_back(c);
            }
        }
        return false;  // 字符串未闭合
    }

    bool ParseHex4(uint32_t* out) {
        if (pos_ + 4 > s_.size()) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        *out = v;
        return true;
    }

    static void AppendUtf8(uint32_t cp, std::string* out) {
        if (cp <= 0x7Fu) {
            out->push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out->push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out->push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out->push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    bool ParseNumber(JVal* v) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            return false;
        }
        bool is_int = true;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            is_int = false;
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            is_int = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        const std::string token = s_.substr(start, pos_ - start);
        char* end = nullptr;
        const double d = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') return false;
        v->k = JVal::K::kNum;
        v->num = d;
        v->num_is_int = false;
        if (is_int) {
            errno = 0;
            char* iend = nullptr;
            const long long i = std::strtoll(token.c_str(), &iend, 10);
            if (iend != nullptr && *iend == '\0' && errno != ERANGE) {
                v->inum = static_cast<int64_t>(i);
                v->num_is_int = true;
            }
        }
        return true;
    }

    bool ParseArray(JVal* v) {
        ++pos_;  // '['
        v->k = JVal::K::kArr;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JVal e;
            if (!ParseValue(&e)) return false;
            v->arr.push_back(std::move(e));
            SkipWs();
            if (pos_ >= s_.size()) return false;
            const char c = s_[pos_++];
            if (c == ']') return true;
            if (c != ',') return false;
            SkipWs();
        }
    }

    bool ParseObject(JVal* v) {
        ++pos_;  // '{'
        v->k = JVal::K::kObj;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return false;
            std::string key;
            if (!ParseString(&key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            SkipWs();
            JVal val;
            if (!ParseValue(&val)) return false;
            v->obj.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (pos_ >= s_.size()) return false;
            const char c = s_[pos_++];
            if (c == '}') return true;
            if (c != ',') return false;
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
};

/// JSON 值 → ColumnValue（按目标列声明类型；null 恒为 NULL）。
bool JValToValue(const JVal& v, rtdb::DataType t, rtdb::ColumnValue* out,
                 std::string* why) {
    if (v.k == JVal::K::kNull) {
        *out = rtdb::ColumnValue::Null();
        return true;
    }
    switch (t) {
        case rtdb::DataType::kUtf8Var:
        case rtdb::DataType::kBytesVar:
            if (v.k != JVal::K::kStr) {
                *why = "变长列须为 JSON 字符串";
                return false;
            }
            *out = (t == rtdb::DataType::kUtf8Var) ? rtdb::ColumnValue::Text(v.str)
                                                  : rtdb::ColumnValue::Bytes(v.str);
            return true;
        case rtdb::DataType::kBool:
            if (v.k == JVal::K::kBool) {
                *out = rtdb::ColumnValue::Bool(v.b);
                return true;
            }
            if (v.k == JVal::K::kNum) {
                *out = rtdb::ColumnValue::Bool(v.num_is_int ? v.inum != 0 : v.num != 0.0);
                return true;
            }
            *why = "布尔列须为 true/false 或数值";
            return false;
        case rtdb::DataType::kFloat32:
        case rtdb::DataType::kFloat64:
            if (v.k != JVal::K::kNum) {
                *why = "浮点列须为数值";
                return false;
            }
            *out = rtdb::ColumnValue::Real(v.num_is_int ? static_cast<double>(v.inum)
                                                       : v.num);
            return true;
        default:
            if (v.k == JVal::K::kNum && v.num_is_int) {
                *out = rtdb::ColumnValue::IntOf(t, v.inum);
                return true;
            }
            if (v.k == JVal::K::kBool) {
                *out = rtdb::ColumnValue::IntOf(t, v.b ? 1 : 0);
                return true;
            }
            *why = "整数/时间戳列须为整数（不接受浮点或字符串）";
            return false;
    }
}

/// JSON 顶层为行数组：每行可为对象（key = 列名）或数组（按 schema 列序）。
/// 未知列、长度不符、类型非法一律在零写入阶段拒绝；缺失列留 NULL。
/// bytes 列按 UTF-8 文本近似处理（任意二进制不保证文本往返，已知边界）。
bool ParseJsonRows(const std::string& text, const rtdb::TableSchema& schema,
                   std::vector<std::vector<rtdb::ColumnValue>>* rows, std::string* why) {
    if (schema.col_names.empty() || schema.col_types.size() != schema.col_names.size()) {
        *why = "表 schema 列名/类型数量不一致";
        return false;
    }
    JVal root;
    JsonParser parser(text);
    if (!parser.Parse(&root) || root.k != JVal::K::kArr) {
        *why = "JSON 顶层须为行数组（[...]）";
        return false;
    }
    rows->clear();
    rows->reserve(root.arr.size());
    for (size_t r = 0; r < root.arr.size(); ++r) {
        const JVal& line = root.arr[r];
        std::vector<rtdb::ColumnValue> row(
            schema.col_names.size(), rtdb::ColumnValue::Null());
        if (line.k == JVal::K::kObj) {
            for (const auto& kv : line.obj) {
                const int ci = FindColumn(schema, kv.first);
                if (ci < 0) {
                    *why = "JSON 第 " + std::to_string(r + 1) + " 行含未知列：" + kv.first;
                    return false;
                }
                std::string err;
                if (!JValToValue(kv.second, schema.col_types[static_cast<size_t>(ci)],
                                 &row[static_cast<size_t>(ci)], &err)) {
                    *why = "JSON 第 " + std::to_string(r + 1) + " 行：" + err;
                    return false;
                }
            }
        } else if (line.k == JVal::K::kArr) {
            if (line.arr.size() != schema.col_names.size()) {
                *why = "JSON 第 " + std::to_string(r + 1) + " 行数组长度与表列数不符";
                return false;
            }
            for (size_t c = 0; c < line.arr.size(); ++c) {
                std::string err;
                if (!JValToValue(line.arr[c], schema.col_types[c], &row[c], &err)) {
                    *why = "JSON 第 " + std::to_string(r + 1) + " 行：" + err;
                    return false;
                }
            }
        } else {
            *why = "JSON 第 " + std::to_string(r + 1) + " 行须为对象或数组";
            return false;
        }
        rows->push_back(std::move(row));
    }
    return true;
}

/// 完整 JSON 字符串转义（数据值）：引号/反斜杠/控制字符，UTF-8 字节原样保留。
std::string JsonStringEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(u));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

std::string JsonValueStr(const rtdb::ColumnValue& v) {
    if (v.is_null()) return "null";
    switch (v.type()) {
        case rtdb::DataType::kFloat32:
        case rtdb::DataType::kFloat64:
            return RealToStr(v.as_real());
        case rtdb::DataType::kUtf8Var:
        case rtdb::DataType::kBytesVar:
            return "\"" + JsonStringEscape(v.as_bytes()) + "\"";
        case rtdb::DataType::kBool:
            return v.as_bool() ? "true" : "false";
        default:
            return std::to_string(v.as_int());
    }
}

/// CSV 单字段：NULL = 空字段；变长值一律加引号（"" 转义）以与空串区分；
/// 数值/布尔裸输出。含逗号/换行/引号的字符串在引号内自然安全。
std::string CsvFieldStr(const rtdb::ColumnValue& v) {
    if (v.is_null()) return std::string();
    switch (v.type()) {
        case rtdb::DataType::kFloat32:
        case rtdb::DataType::kFloat64:
            return RealToStr(v.as_real());
        case rtdb::DataType::kUtf8Var:
        case rtdb::DataType::kBytesVar: {
            std::string out;
            out.reserve(v.as_bytes().size() + 2);
            out.push_back('"');
            for (char c : v.as_bytes()) {
                if (c == '"') out.push_back('"');  // "" 转义
                out.push_back(c);
            }
            out.push_back('"');
            return out;
        }
        case rtdb::DataType::kBool:
            return v.as_bool() ? "true" : "false";
        default:
            return std::to_string(v.as_int());
    }
}

std::string CsvRowStr(const std::vector<rtdb::ColumnValue>& row) {
    std::string out;
    for (size_t i = 0; i < row.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += CsvFieldStr(row[i]);
    }
    out.push_back('\n');
    return out;
}

std::string CsvHeaderStr(const rtdb::TableSchema& schema) {
    std::string out;
    for (size_t i = 0; i < schema.col_names.size(); ++i) {
        if (i != 0) out.push_back(',');
        out.push_back('"');
        for (char c : schema.col_names[i]) {
            if (c == '"') out.push_back('"');
            out.push_back(c);
        }
        out.push_back('"');
    }
    out.push_back('\n');
    return out;
}

std::string JsonRowStr(const rtdb::TableSchema& schema,
                       const std::vector<rtdb::ColumnValue>& row) {
    const size_t n = schema.col_names.size() < row.size() ? schema.col_names.size()
                                                        : row.size();
    std::string out = "{";
    for (size_t i = 0; i < n; ++i) {
        if (i != 0) out.push_back(',');
        out.push_back('"');
        out += JsonStringEscape(schema.col_names[i]);
        out += "\":";
        out += JsonValueStr(row[i]);
    }
    out.push_back('}');
    return out;
}

/// import：先全量解析 + 校验（输入错误一律零写入暴露），再按批预算分片，
/// 逐批 ExecuteBatch 原子写。总行数 ≤ 批预算时整文件只发一个原子批，
/// 失败整体零副作用；超过批预算必须分片，批间无法全局原子（已知边界，
/// 失败时明确报告已写入的前缀批，提示以 export 核对）。
int Import(rtdb::Client& cli, const std::string& table, const Args& args) {
    if (args.file.empty()) {
        std::fprintf(stderr, "import 需要 --file <path>\n");
        return 2;
    }
    std::string text;
    std::string why;
    if (!ReadAllFile(args.file, &text, &why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }
    auto sc = cli.DescribeTable(table);
    if (!sc.IsOk()) {
        std::fprintf(stderr, "describe 失败：%s\n", rtdb::ErrorMessage(sc.Error()));
        return 1;
    }
    const rtdb::TableSchema schema = sc.Value();
    std::vector<std::vector<rtdb::ColumnValue>> rows;
    const bool json_input = IsJsonInput(args.file, text);
    const bool ok = json_input ? ParseJsonRows(text, schema, &rows, &why)
                               : ParseCsvRows(text, schema, &rows, &why);
    if (!ok) {
        std::fprintf(stderr, "解析 %s 失败（%s）：%s\n", args.file.c_str(),
                     json_input ? "JSON" : "CSV", why.c_str());
        return 2;
    }
    const uint32_t budget = args.batch_rows != 0 ? args.batch_rows : kImportBatchRows;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t imported = 0;
    uint32_t batches = 0;
    for (size_t off = 0; off < rows.size(); off += budget) {
        const size_t end = (off + budget < rows.size()) ? off + budget : rows.size();
        std::vector<rtdb::ClientBatchOp> ops;
        ops.reserve(end - off);
        for (size_t k = off; k < end; ++k) {
            rtdb::ClientBatchOp op;
            op.type = rtdb::ClientBatchOp::Type::kInsert;
            op.table = table;
            op.row = rows[k];
            ops.push_back(std::move(op));
        }
        auto r = cli.ExecuteBatch(ops);
        if (!r.IsOk()) {
            std::fprintf(stderr, "导入第 %u 批失败（已写入 %llu 行/%u 批）：%s\n",
                         batches + 1, static_cast<unsigned long long>(imported), batches,
                         rtdb::ErrorMessage(r.Error()));
            return 1;
        }
        imported += static_cast<uint64_t>(end - off);
        ++batches;
    }
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    if (args.json) {
        std::printf("{\"rows\":%llu,\"batches\":%u,\"elapsed_ms\":%.3f}\n",
                    static_cast<unsigned long long>(imported), batches, elapsed_ms);
    } else {
        std::printf("导入完成：%llu 行 / %u 批 / %.3f ms\n",
                    static_cast<unsigned long long>(imported), batches, elapsed_ms);
    }
    return 0;
}

/// export：分页 ScanPaged 扫描（逐页回调，绝不一次物化全表），输出到 stdout
/// 或 --file；CSV 带表头、JSON 为行数组。失败时删除本命令刚创建的输出文件，
/// 不留半成品输出。统计信息写 stderr，避免污染 stdout 数据流。
int Export(rtdb::Client& cli, const std::string& table, const Args& args) {
    const bool to_file = !args.file.empty();
    std::ofstream ofs;
    if (to_file) {
        ofs.open(args.file, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            std::fprintf(stderr, "无法写入文件：%s\n", args.file.c_str());
            return 1;
        }
    }
    auto emit = [&](const std::string& s) {
        if (to_file) {
            ofs.write(s.data(), static_cast<std::streamsize>(s.size()));
        } else {
            std::fwrite(s.data(), 1, s.size(), stdout);
        }
    };
    // 先 DESCRIBE 取列名：空表也能输出完整的 CSV 表头 / JSON 数组结构。
    auto sc = cli.DescribeTable(table);
    if (!sc.IsOk()) {
        std::fprintf(stderr, "describe 失败：%s\n", rtdb::ErrorMessage(sc.Error()));
        if (to_file) {
            ofs.close();
            std::remove(args.file.c_str());
        }
        return 1;
    }
    const rtdb::TableSchema schema = sc.Value();
    if (args.json) {
        emit("[");
    } else {
        emit(CsvHeaderStr(schema));
    }
    uint64_t written = 0;
    auto r = cli.ScanPaged(
        table, kExportPageRows, [&](const rtdb::ClientScanResult& page) -> bool {
            for (size_t i = 0; i < page.rows.size(); ++i) {
                if (args.json) {
                    std::string line = (written == 0) ? "" : ",";
                    line += JsonRowStr(schema, page.rows[i]);
                    emit(line);
                } else {
                    emit(CsvRowStr(page.rows[i]));
                }
                ++written;
            }
            return true;  // 继续下一页
        });
    if (!r.IsOk()) {
        std::fprintf(stderr, "分页扫描失败：%s\n", rtdb::ErrorMessage(r.Error()));
        if (to_file) {
            ofs.close();
            std::remove(args.file.c_str());
        }
        return 1;
    }
    if (args.json) emit("]\n");
    if (to_file) {
        ofs.flush();
        if (!ofs) {
            std::fprintf(stderr, "写文件失败：%s\n", args.file.c_str());
            ofs.close();
            std::remove(args.file.c_str());
            return 1;
        }
        ofs.close();
    } else {
        std::fflush(stdout);
    }
    std::fprintf(stderr, "导出完成：%llu 行\n",
                 static_cast<unsigned long long>(written));
    return 0;
}

// ---- subscribe：输出格式与 examples/remote_demo 一致，便于双端比对 ----

const char* ChangeKindName(rtdb::ChangeEvent::Kind k) {
    switch (k) {
        case rtdb::ChangeEvent::Kind::kInsert:
            return "insert";
        case rtdb::ChangeEvent::Kind::kUpdate:
            return "update";
        case rtdb::ChangeEvent::Kind::kDelete:
            return "delete";
    }
    return "?";
}

std::string ChangeValueToStr(const rtdb::ColumnValue& v) {
    if (v.is_null()) return "NULL";
    switch (v.type()) {
        case rtdb::DataType::kFloat32:
        case rtdb::DataType::kFloat64:
            return std::to_string(v.as_real());
        case rtdb::DataType::kUtf8Var:
        case rtdb::DataType::kBytesVar:
            return std::string(v.as_bytes());
        default:
            return std::to_string(v.as_int());
    }
}

std::string ChangeRowToStr(const std::vector<rtdb::ColumnValue>& row) {
    std::string s = "[";
    for (size_t i = 0; i < row.size(); ++i) {
        if (i != 0) s += ", ";
        s += ChangeValueToStr(row[i]);
    }
    return s + "]";
}

/// subscribe：订阅打印（客户端保证 seq 严格递增交付），--seconds 限时或
/// Ctrl+C/SIGTERM 优雅退出；溢出按 remote_demo 路径整表重取 + ResetTo。
int Subscribe(rtdb::Client& cli, const std::string& table, const Args& args) {
    std::atomic<uint64_t> received{0};
    uint64_t last_seq = 0;  // 仅泵线程读写（回调内），用于防御性 seq 回退检测
    rtdb::SubscriptionOptions so;
    so.tables = {table};
    so.delta_wait_ms = 1000;
    so.on_state = [](bool ready, uint32_t attempt) {
        if (ready) {
            std::printf("订阅就绪\n");
        } else {
            std::printf("订阅重连中（第 %u 次）…\n", attempt);
        }
    };
    so.on_change = [&received, &last_seq](const rtdb::ChangeEvent& ev) {
        if (ev.seq < last_seq) {
            std::fprintf(stderr, "警告：订阅 seq 回退 %llu -> %llu\n",
                        static_cast<unsigned long long>(last_seq),
                        static_cast<unsigned long long>(ev.seq));
        } else {
            last_seq = ev.seq;
        }
        const uint64_t n = received.fetch_add(1) + 1;
        if (ev.kind == rtdb::ChangeEvent::Kind::kDelete) {
            std::printf("#%llu seq=%llu %s 表=%s 主键=%s\n",
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(ev.seq), ChangeKindName(ev.kind),
                        ev.table.c_str(), ChangeValueToStr(ev.key).c_str());
        } else {
            std::printf("#%llu seq=%llu %s 表=%s 行=%s\n",
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(ev.seq), ChangeKindName(ev.kind),
                        ev.table.c_str(), ChangeRowToStr(ev.row).c_str());
        }
    };
    so.on_overflow = [&cli, &table](rtdb::Subscription& sub) {
        std::printf("订阅游标越窗（历史被环回收）：整表重取 + 重置游标\n");
        uint64_t committed = 0;
        auto p = cli.Ping();
        if (p.IsOk() && p.Value().committed_seq >= 0) {
            committed = static_cast<uint64_t>(p.Value().committed_seq);
        }
        uint64_t rows = 0;
        auto s = cli.ScanPaged(table, kExportPageRows,
                              [&rows](const rtdb::ClientScanResult& page) {
                                  rows += page.rows.size();
                                  return true;
                              });
        if (s.IsOk()) {
            std::printf("重扫完成：%llu 行；游标重置到 seq=%llu\n",
                        static_cast<unsigned long long>(s.Value()),
                        static_cast<unsigned long long>(committed));
        } else {
            std::fprintf(stderr, "重扫失败：%s\n", rtdb::ErrorMessage(s.Error()));
        }
        sub.ResetTo(committed);
    };
    auto sr = cli.Subscribe(so);
    if (!sr.IsOk()) {
        std::fprintf(stderr, "订阅失败：%s\n", rtdb::ErrorMessage(sr.Error()));
        return 1;
    }
    std::unique_ptr<rtdb::Subscription> sub = sr.TakeValue();
    if (!sub->WaitReady(5000)) {
        std::fprintf(stderr, "订阅未在预算内就绪：%s\n", sub->last_error().c_str());
    }
    const auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (args.seconds != 0 &&
            std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(args.seconds)) {
            break;
        }
    }
    std::printf("已收变更 %llu 条；关闭订阅与连接…\n",
                static_cast<unsigned long long>(received.load()));
    sub->Close();
    cli.Close();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    rtdb::platform::UseUtf8Console();  // 中文 Windows 控制台：UTF-8 输出不乱码（docs/18 §6）
    std::signal(SIGINT, OnSignal);   // Ctrl+C 优雅退出（subscribe 循环检查 g_stop）
    std::signal(SIGTERM, OnSignal);
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* out) -> bool {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        if (a == "--host") {
            if (!next(&args.host)) { Usage(); return 2; }
        } else if (a == "--port") {
            if (!next(&args.port_str) || !ParsePort(args.port_str, &args.port)) {
                Usage();
                return 2;
            }
        } else if (a == "--user") {
            if (!next(&args.user)) { Usage(); return 2; }
        } else if (a == "--password") {
            if (!next(&args.password)) { Usage(); return 2; }
        } else if (a == "--salt") {
            if (!next(&args.salt)) { Usage(); return 2; }
        } else if (a == "--json") {
            args.json = true;
        } else if (a == "--col") {
            std::string v;
            if (!next(&v)) { Usage(); return 2; }
            args.cols.push_back(std::move(v));
        } else if (a == "--row-cap") {
            std::string v;
            if (!next(&v) || !ParseU32(v, &args.row_cap)) { Usage(); return 2; }
        } else if (a == "--file") {
            if (!next(&args.file)) { Usage(); return 2; }
        } else if (a == "--seconds") {
            std::string v;
            if (!next(&v) || !ParseU32(v, &args.seconds)) { Usage(); return 2; }
        } else if (a == "--batch") {
            std::string v;
            if (!next(&v) || !ParseU32(v, &args.batch_rows)) { Usage(); return 2; }
        } else if (a == "--help" || a == "-h") {
            Usage();
            return 0;
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "未知参数：%s\n", a.c_str());
            Usage();
            return 2;
        } else {
            args.pos.push_back(a);
        }
    }

    if (args.pos.empty()) {
        Usage();
        return 2;
    }
    if (args.port == 0 || args.user.empty()) {
        std::fprintf(stderr, "缺少 --port 或 --user\n");
        Usage();
        return 2;
    }

    rtdb::ClientOptions opts;
    opts.host = args.host;
    opts.port = args.port;
    opts.user = args.user;
    opts.password = args.password;
    opts.auth_salt = args.salt;
    auto cli = rtdb::Client::Connect(opts);
    if (!cli.IsOk()) {
        std::fprintf(stderr, "连接/认证失败：%s\n", rtdb::ErrorMessage(cli.Error()));
        return 1;
    }

    const std::string& cmd = args.pos[0];
    if (cmd == "status") {
        return PrintStats(*cli.Value(), args.json);
    }
    if (cmd == "authz") {
        if (args.pos.size() < 2) {
            Usage();
            return 2;
        }
        const std::string& sub = args.pos[1];
        if (sub == "me") return PrintAuthz(*cli.Value(), args.json);
        if (sub == "show") return PrintAuthzShow(*cli.Value(), args.json);
        if (sub == "check") {
            if (args.pos.size() < 3) {
                Usage();
                return 2;
            }
            std::string mode = "r";  // 省略时默认检查读权限
            if (args.pos.size() >= 4) {
                mode = Lower(args.pos[3]);
                if (mode != "r" && mode != "w") {
                    Usage();
                    return 2;
                }
            }
            return AuthzCheck(*cli.Value(), args.pos[2], mode, args.json);
        }
        Usage();
        return 2;
    }
    if (cmd == "ddl") {
        if (args.pos.size() < 3) {
            Usage();
            return 2;
        }
        const std::string& sub = args.pos[1];
        const std::string& table = args.pos[2];
        if (sub == "describe") return DdlDescribe(*cli.Value(), table, args.json);
        if (sub == "drop") {
            auto r = cli.Value()->DropTable(table);
            if (!r.IsOk()) {
                std::fprintf(stderr, "删表失败：%s\n", rtdb::ErrorMessage(r.Error()));
                return 1;
            }
            std::printf("已删除表 %s\n", table.c_str());
            return 0;
        }
        if (sub == "create") return DdlCreate(*cli.Value(), table, args);
        Usage();
        return 2;
    }
    if (cmd == "import" || cmd == "export" || cmd == "subscribe") {
        if (args.pos.size() < 2) {
            Usage();
            return 2;
        }
        const std::string& table = args.pos[1];
        if (cmd == "import") return Import(*cli.Value(), table, args);
        if (cmd == "export") return Export(*cli.Value(), table, args);
        return Subscribe(*cli.Value(), table, args);
    }
    Usage();
    return 2;
}
