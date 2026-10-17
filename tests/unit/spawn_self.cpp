#include "tests/unit/spawn_self.hpp"

#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rtdb_test {

void SetEnvVar(const char* name, const char* value) { ::_putenv_s(name, value); }
void UnsetEnvVar(const char* name) { ::_putenv_s(name, ""); }

namespace {

/// 以 [exe, args...] 组装 CreateProcessA 命令行（exe 与各参数均加引号）。
SpawnOutcome SpawnImpl(const std::string& exe, const std::vector<std::string>& args) {
    SpawnOutcome out;
    std::string s = "\"" + exe + "\"";
    for (const std::string& a : args) s += " \"" + a + "\"";
    std::vector<char> cmdline(s.begin(), s.end());
    cmdline.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &si, &pi) == 0)
        return out;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    out.spawned = true;
    out.exit_code = static_cast<int>(code);
    return out;
}

}  // namespace

SpawnOutcome SpawnSelfWithFlag(const std::string& exe, const std::string& extra_arg) {
    return SpawnImpl(exe, {kCrashLockFlag, extra_arg});
}

SpawnOutcome SpawnSelf(const std::string& exe, const std::vector<std::string>& args) {
    return SpawnImpl(exe, args);
}

// ---------------- ChildProcess（异步硬杀；M3-W5） ----------------

ChildProcess::~ChildProcess() {
    if (spawned_ && !reaped_) {
        Kill9();
        (void)Wait();
    }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : spawned_(other.spawned_), reaped_(other.reaped_), handle_(other.handle_) {
    other.spawned_ = false;
    other.reaped_ = true;
    other.handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        if (spawned_ && !reaped_) {
            Kill9();
            (void)Wait();
        }
        spawned_ = other.spawned_;
        reaped_ = other.reaped_;
        handle_ = other.handle_;
        other.spawned_ = false;
        other.reaped_ = true;
        other.handle_ = nullptr;
    }
    return *this;
}

ChildProcess ChildProcess::Spawn(const std::string& exe, const std::vector<std::string>& args) {
    ChildProcess out;
    std::string s = "\"" + exe + "\"";
    for (const std::string& a : args) s += " \"" + a + "\"";
    std::vector<char> cmdline(s.begin(), s.end());
    cmdline.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                       &pi) == 0) {
        return out;
    }
    CloseHandle(pi.hThread);
    out.spawned_ = true;
    out.handle_ = pi.hProcess;
    return out;
}

bool ChildProcess::alive() noexcept {
    if (!spawned_ || reaped_) return false;
    const DWORD r = WaitForSingleObject(static_cast<HANDLE>(handle_), 0);
    if (r == WAIT_TIMEOUT) return true;
    if (r == WAIT_OBJECT_0) {
        (void)Wait();  // 已退出：回收
        return false;
    }
    return false;
}

void ChildProcess::Kill9() noexcept {
    if (!spawned_ || reaped_) return;
    (void)TerminateProcess(static_cast<HANDLE>(handle_), 9);
}

int ChildProcess::Wait() noexcept {
    if (!spawned_) return -1;
    if (reaped_) return -1;
    (void)WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    reaped_ = true;
    // TerminateProcess(...,9) 返回 9；正常退出返回进程码。
    return static_cast<int>(code);
}

}  // namespace rtdb_test

#else

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

namespace rtdb_test {

void SetEnvVar(const char* name, const char* value) { ::setenv(name, value, 1); }
void UnsetEnvVar(const char* name) { ::unsetenv(name); }

namespace {

SpawnOutcome SpawnImpl(const std::string& exe, const std::vector<std::string>& args) {
    SpawnOutcome out;
    const pid_t pid = ::fork();
    if (pid < 0) return out;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);  // exec 失败
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    out.spawned = true;
    out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return out;
}

}  // namespace

SpawnOutcome SpawnSelfWithFlag(const std::string& exe, const std::string& extra_arg) {
    return SpawnImpl(exe, {std::string(kCrashLockFlag), extra_arg});
}

SpawnOutcome SpawnSelf(const std::string& exe, const std::vector<std::string>& args) {
    return SpawnImpl(exe, args);
}

// ---------------- ChildProcess（异步硬杀；M3-W5） ----------------

ChildProcess::~ChildProcess() {
    if (spawned_ && !reaped_) {
        Kill9();
        (void)Wait();
    }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : spawned_(other.spawned_), reaped_(other.reaped_), pid_(other.pid_) {
    other.spawned_ = false;
    other.reaped_ = true;
    other.pid_ = -1;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        if (spawned_ && !reaped_) {
            Kill9();
            (void)Wait();
        }
        spawned_ = other.spawned_;
        reaped_ = other.reaped_;
        pid_ = other.pid_;
        other.spawned_ = false;
        other.reaped_ = true;
        other.pid_ = -1;
    }
    return *this;
}

ChildProcess ChildProcess::Spawn(const std::string& exe, const std::vector<std::string>& args) {
    ChildProcess out;
    // 多线程安全：argv 全部在 fork 前于父进程构造——fork 后子进程仅允许
    // 异步信号安全调用（在子进程内调 malloc 可能与其他线程持有的分配器
    // 锁死锁；混沌场景 2 正是在写线程运行中重启服务端）。字符串本体在
    // 父进程生命周期内稳定，fork 后子进程只读。
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) return out;
    if (pid == 0) {
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);  // exec 失败
    }
    out.spawned_ = true;
    out.pid_ = static_cast<long long>(pid);
    return out;
}

bool ChildProcess::alive() noexcept {
    if (!spawned_ || reaped_) return false;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == 0) return true;
    // 已退出（或出错）：按已回收处理并丢弃退出码（Wait 已无法取值）。
    reaped_ = true;
    pid_ = -1;
    return false;
}

void ChildProcess::Kill9() noexcept {
    if (!spawned_ || reaped_) return;
    (void)::kill(static_cast<pid_t>(pid_), SIGKILL);
}

int ChildProcess::Wait() noexcept {
    if (!spawned_ || reaped_) return -1;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    reaped_ = true;
    pid_ = -1;
    if (r < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);  // SIGKILL → 137
    return -1;
}

}  // namespace rtdb_test

#endif
