/// \file 进程身份/活性检测 —— Windows 实现。
/// 启动时间取 GetProcessTimes 的 CreationTime（FILETIME 100ns -> ns）。
/// 防呆：CreationTime 同一进程内恒定，跨进程比较安全。

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
  if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user))
    return 0;
  return FileTimeToNs(create);
}

bool IsSameLiveProcess(uint32_t pid, uint64_t start_time_ns) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (h == nullptr) return false;
  FILETIME create{}, exit{}, kernel{}, user{};
  const bool matched =
      GetProcessTimes(h, &create, &exit, &kernel, &user) &&
      FileTimeToNs(create) == start_time_ns;
  CloseHandle(h);
  return matched;
}

std::string HostExecutablePath() {
  char buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  return std::string(buf, n > 0 ? n : 0);
}

}  // namespace platform
}  // namespace rtdb
