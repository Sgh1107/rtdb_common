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

}  // namespace rtdb_test

#else

#include <sys/wait.h>
#include <unistd.h>

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

}  // namespace rtdb_test

#endif
