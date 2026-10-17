/// \file 进程身份/活性检测 —— Windows 实现。
/// 启动时间取 GetProcessTimes 的 CreationTime（FILETIME 100ns -> ns）。
/// 防呆：CreationTime 同一进程内恒定，跨进程比较安全。
///
/// 重要事实（M3-W5 混沌加固）：进程被 TerminateProcess/ExitProcess 终止后，
/// 其**进程对象可能短暂滞留**（PID 句柄/内核引用清零前仍可 OpenProcess），
/// 此时 CreationTime 与存活进程相同——若只看 CreationTime 会把"已终止但
/// 滞留"的持有者误判为存活，导致共享内存锁的死亡恢复在超时前无法接管
/// （rtdbd 硬杀后 ~120ms 内重启即 5s 超时复现）。判据补强：GetProcessTimes
/// 的 ExitTime 非零即"已退出"，优先于 CreationTime 匹配返回。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "platform/platform_process.hpp"

namespace rtdb {
namespace platform {

namespace {

uint64_t FileTimeToNs(const FILETIME& ft) noexcept {
    ULARGE_INTEGER ul{};
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    return static_cast<uint64_t>(ul.QuadPart) * 100ull;  // 100ns -> 1ns
}

}  // namespace

uint32_t CurrentProcessId() { return static_cast<uint32_t>(GetCurrentProcessId()); }

uint64_t CurrentProcessStartTimeNs() {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return 0;
    return FileTimeToNs(create);
}

bool IsSameLiveProcess(uint32_t pid, uint64_t start_time_ns) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return false;
    FILETIME create{}, exit{}, kernel{}, user{};
    const bool ok = GetProcessTimes(h, &create, &exit, &kernel, &user) != 0;
    CloseHandle(h);
    if (!ok) return false;
    if (FileTimeToNs(exit) != 0) return false;  // 已退出（对象滞留窗口）→ 死
    return FileTimeToNs(create) == start_time_ns;
}

std::string HostExecutablePath() {
    char buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, n > 0 ? n : 0);
}

}  // namespace platform
}  // namespace rtdb
