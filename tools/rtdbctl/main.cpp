/// \file
/// \brief M4 rtdbctl 运维 CLI（docs/16 D9）：W1 交付 status/authz me，
/// W2 追加 ddl create/describe/drop。连接/鉴权参数与 bench_net 一致
/// （口令摘要 = SHA-256(salt || password)）。
///
/// 用法：
///   rtdbctl status [--json] [连接参数]
///   rtdbctl authz me [--json] [连接参数]
///   rtdbctl ddl describe <table> [--json] [连接参数]
///   rtdbctl ddl drop <table> [连接参数]
///   rtdbctl ddl create <table> --col <name:type[:nn|:null]>... [--row-cap N] [连接参数]
///
/// 退出码：0 = 成功；1 = 请求/连接失败；2 = 用法错误。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
    std::vector<std::string> pos;
    std::vector<std::string> cols;
};

void Usage() {
    std::fprintf(stderr,
                 "用法：rtdbctl <command> [选项]\n"
                 "  status [--json]\n"
                 "  authz me [--json]\n"
                 "  ddl describe <table> [--json]\n"
                 "  ddl drop <table>\n"
                 "  ddl create <table> --col <name:type[:nn|:null]>... [--row-cap N]\n"
                 "连接：--host H --port P --user U --password PW --salt S\n"
                 "type：int8/int16/int32/int64/bool/float32/float64/utf8/bytes/ts\n"
                 "首个 --col 为主键（恒 NOT NULL）；其余列默认 nullable。\n");
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
        std::printf("{\"user\":\"%s\",\"admin\":%s,\"ddl\":%s,\"read\":[",
                    JsonEscape(ai.user).c_str(), ai.admin ? "true" : "false",
                    ai.ddl ? "true" : "false");
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

}  // namespace

int main(int argc, char** argv) {
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
        if (args.pos.size() < 2 || args.pos[1] != "me") {
            Usage();
            return 2;
        }
        return PrintAuthz(*cli.Value(), args.json);
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
    Usage();
    return 2;
}
