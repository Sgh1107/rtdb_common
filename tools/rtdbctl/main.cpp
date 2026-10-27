/// \file
/// \brief M4 rtdbctl 运维 CLI（docs/16 D9；W1 先交付 status/authz）。
///
/// 用法：
///   rtdbctl status [--host H] [--port P] [--user U] [--password PW] [--salt S] [--json]
///   rtdbctl authz me [同上]
///
/// 连接/鉴权参数与 bench_net 一致（口令摘要 = SHA-256(salt || password)）。
/// 退出码：0 = 成功；1 = 请求/连接失败；2 = 用法错误。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rtdb/err.hpp"
#include "rtdb/rtdb_client.hpp"

namespace {

void Usage() {
    std::fprintf(stderr,
                 "用法：rtdbctl <status|authz me> [--host H] [--port P] [--user U]\n"
                 "               [--password PW] [--salt S] [--json]\n");
}

bool ParsePort(const std::string& s, uint16_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v > 65535) return false;
    *out = static_cast<uint16_t>(v);
    return true;
}

/// 极简 JSON 字符串转义（指标名/表名为标识符，只需处理引号/反斜杠）。
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string cmd;
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    std::string user;
    std::string password;
    std::string salt;
    bool json = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* out) -> bool {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        if (a == "--host") {
            if (!next(&host)) { Usage(); return 2; }
        } else if (a == "--port") {
            std::string v;
            if (!next(&v) || !ParsePort(v, &port)) { Usage(); return 2; }
        } else if (a == "--user") {
            if (!next(&user)) { Usage(); return 2; }
        } else if (a == "--password") {
            if (!next(&password)) { Usage(); return 2; }
        } else if (a == "--salt") {
            if (!next(&salt)) { Usage(); return 2; }
        } else if (a == "--json") {
            json = true;
        } else if (a == "--help" || a == "-h") {
            Usage();
            return 0;
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "未知参数：%s\n", a.c_str());
            Usage();
            return 2;
        } else if (cmd.empty()) {
            cmd = a;
        } else if (cmd == "authz" && a == "me") {
            cmd = "authz-me";
        } else {
            Usage();
            return 2;
        }
    }

    if (cmd != "status" && cmd != "authz" && cmd != "authz-me") {
        Usage();
        return 2;
    }
    if (port == 0 || user.empty()) {
        std::fprintf(stderr, "缺少 --port 或 --user\n");
        Usage();
        return 2;
    }

    rtdb::ClientOptions opts;
    opts.host = host;
    opts.port = port;
    opts.user = user;
    opts.password = password;
    opts.auth_salt = salt;
    auto cli = rtdb::Client::Connect(opts);
    if (!cli.IsOk()) {
        std::fprintf(stderr, "连接/认证失败：%s\n", rtdb::ErrorMessage(cli.Error()));
        return 1;
    }

    if (cmd == "status") {
        auto st = cli.Value()->GetServerStats();
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

    // authz me（authz 未带 me 时同样展示当前用户权限摘要）
    auto info = cli.Value()->GetAuthzInfo();
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
