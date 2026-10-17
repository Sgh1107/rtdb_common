/// \file
/// \brief rtdbd 进程入口：CLI 覆盖配置、信号处理与优雅退出（docs/13 §5-W3）。
///
/// 用法：
///   rtdbd --config <cfg> [--listen host:port] [--path <映像>] [--instance <名>]
///         [--auth-salt <盐>] [--user <名=64hex>]... [--exit-after-ms <n>]
///   rtdbd --hash-password <口令> [--salt <盐>]
///
/// `--user` 的摘要 = SHA-256(salt || password)，由 `--hash-password` 生成
/// （部署离线算好写进配置；盐与摘要都不构成机密，docs/13 §3-D6）。
/// `--exit-after-ms` 供脚本/冒烟测试：到时优雅退出（退出码 0）。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "infra/sha256.hpp"
#include "rtdb/err.hpp"
#include "server/config.hpp"
#include "server/server.hpp"

namespace {

std::atomic<bool> g_stop{false};

void OnStopSignal(int /*sig*/) { g_stop.store(true); }

#if defined(_WIN32)
BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}
#endif

void PrintUsage() {
    std::fprintf(stderr,
                 "用法：rtdbd --config <cfg> [--listen HOST:PORT] [--path IMG] [--instance NAME]\n"
                 "             [--auth-salt SALT] [--user NAME=64HEX]... [--exit-after-ms N]\n"
                 "      rtdbd --hash-password PASSWORD [--salt SALT]\n");
}

bool ParseU32(const std::string& s, uint32_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v > 0xFFFFFFFFul) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::string listen;
    std::string path;
    std::string instance;
    std::string auth_salt;
    bool auth_salt_set = false;
    std::vector<std::string> users;
    std::string hash_password;
    std::string hash_salt;
    bool hash_salt_set = false;
    bool hash_mode = false;
    uint32_t exit_after_ms = 0;

    auto take_value = [&](int* i, std::string* out) -> bool {
        if (*i + 1 >= argc) return false;
        *out = argv[++(*i)];
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        std::string v;
        if (a == "--config" || a == "-c") {
            if (!take_value(&i, &config_path)) {
                PrintUsage();
                return 2;
            }
        } else if (a == "--listen") {
            if (!take_value(&i, &listen)) {
                PrintUsage();
                return 2;
            }
        } else if (a == "--path") {
            if (!take_value(&i, &path)) {
                PrintUsage();
                return 2;
            }
        } else if (a == "--instance") {
            if (!take_value(&i, &instance)) {
                PrintUsage();
                return 2;
            }
        } else if (a == "--auth-salt") {
            if (!take_value(&i, &auth_salt)) {
                PrintUsage();
                return 2;
            }
            auth_salt_set = true;
        } else if (a == "--user") {
            if (!take_value(&i, &v)) {
                PrintUsage();
                return 2;
            }
            users.push_back(std::move(v));
        } else if (a == "--hash-password") {
            if (!take_value(&i, &hash_password)) {
                PrintUsage();
                return 2;
            }
            hash_mode = true;
        } else if (a == "--salt") {
            if (!take_value(&i, &hash_salt)) {
                PrintUsage();
                return 2;
            }
            hash_salt_set = true;
        } else if (a == "--exit-after-ms") {
            if (!take_value(&i, &v) || !ParseU32(v, &exit_after_ms)) {
                PrintUsage();
                return 2;
            }
        } else if (a == "--help" || a == "-h") {
            PrintUsage();
            return 0;
        } else {
            std::fprintf(stderr, "未知参数：%s\n", a.c_str());
            PrintUsage();
            return 2;
        }
    }

    if (hash_mode) {
        const std::string salt =
            hash_salt_set ? hash_salt : (auth_salt_set ? auth_salt : std::string());
        const std::string hex = rtdb::server::DigestHex(
            rtdb::server::HashPassword(salt, hash_password));
        std::printf("%s\n", hex.c_str());
        return 0;
    }

    rtdb::server::Config cfg;
    if (!config_path.empty()) {
        auto c = rtdb::server::LoadConfigFile(config_path);
        if (!c.IsOk()) {
            std::fprintf(stderr, "配置读取失败（%s）：%s\n", config_path.c_str(),
                         rtdb::ErrorMessage(c.Error()));
            return 1;
        }
        cfg = c.TakeValue();
    }
    if (!listen.empty()) {
        std::string host;
        uint16_t port = 0;
        if (!rtdb::IsOk(rtdb::server::ParseListen(listen, &host, &port))) {
            std::fprintf(stderr, "非法监听地址：%s\n", listen.c_str());
            return 2;
        }
        cfg.listen_host = host;
        cfg.listen_port = port;
    }
    if (!path.empty()) cfg.path = path;
    if (!instance.empty()) cfg.instance = instance;
    if (auth_salt_set) cfg.auth_salt = auth_salt;
    for (const std::string& u : users) {
        const size_t sep = u.find_first_of("=:");
        if (sep == std::string::npos || sep == 0) {
            std::fprintf(stderr, "非法 --user（期望 NAME=64HEX）：%s\n", u.c_str());
            return 2;
        }
        rtdb::server::UserEntry e;
        e.name = u.substr(0, sep);
        if (!rtdb::infra::FromHex(std::string_view(u).substr(sep + 1), e.digest.data(),
                                  e.digest.size())) {
            std::fprintf(stderr, "非法 --user 摘要（须 64 位十六进制）：%s\n", u.c_str());
            return 2;
        }
        cfg.users.push_back(std::move(e));
    }
    if (cfg.path.empty()) {
        std::fprintf(stderr, "缺少映像路径（--path 或配置文件 path=）\n");
        return 2;
    }

#if !defined(_WIN32)
    struct sigaction sa {};
    sa.sa_handler = OnStopSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);
    (void)signal(SIGPIPE, SIG_IGN);
#else
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#endif

    rtdb::server::Server srv;
    const rtdb::Err e = srv.Start(cfg);
    if (!rtdb::IsOk(e)) {
        std::fprintf(stderr, "启动失败：%s\n", rtdb::ErrorMessage(e));
        return 1;
    }
    std::printf("rtdbd 就绪：监听 %s:%u 映像 %s（instance=%s，用户 %zu 个）\n",
                cfg.listen_host.c_str(), static_cast<unsigned>(srv.port()), cfg.path.c_str(),
                cfg.instance.c_str(), cfg.users.size());
    std::fflush(stdout);

    const auto deadline =
        exit_after_ms > 0
            ? std::chrono::steady_clock::now() + std::chrono::milliseconds(exit_after_ms)
            : std::chrono::steady_clock::time_point::max();
    while (!g_stop.load()) {
        if (exit_after_ms > 0 && std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    srv.Stop();
    const rtdb::server::Server::Stats st = srv.stats();
    std::printf("rtdbd 退出：接受 %llu，拒绝 %llu，请求 %llu，帧错 %llu\n",
                static_cast<unsigned long long>(st.accepted),
                static_cast<unsigned long long>(st.rejected_full),
                static_cast<unsigned long long>(st.requests),
                static_cast<unsigned long long>(st.frame_errors));
    return 0;
}
