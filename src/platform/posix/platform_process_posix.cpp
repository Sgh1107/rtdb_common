/// \file 进程身份/活性检测 —— POSIX/Linux 实现。
/// 启动时间从 /proc/<pid>/stat 第 22 字段 starttime（jiffies）换算；
/// 无 /proc 的 POSIX 平台退化为 kill(pid,0) 存活检查（尽力而为）。

#ifndef _WIN32

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "platform/platform_process.hpp"

#if defined(__linux__)
#include <sys/sysinfo.h>  // sysconf(_SC_CLK_TCK) 所需 <unistd.h> 已含
#define RTDB_HAS_PROCFS 1
#endif

namespace rtdb {
namespace platform {

namespace {

#if defined(RTDB_HAS_PROCFS)
/// 读取目标进程 starttime(jiffies)。/proc/<pid>/stat 的可执行名可能
/// 含空格/括号，以最后一个 ')' 为界解析其后字段。
bool ReadProcStartJiffies(pid_t pid, unsigned long long* out) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    char buf[2048];
    const char* line = std::fgets(buf, sizeof(buf), f);
    std::fclose(f);
    if (line == nullptr) return false;
    const char* p = std::strrchr(buf, ')');
    if (p == nullptr) return false;
    // ')' 后跳过空格进入 state 字段；starttime 是 state 后第 19 个字段。
    // 注意：被抑制的转换(%*) 不需要长度修饰符（%*llu 属 GNU 扩展会触发
    // -Wformat 警告），直接用 %*d/%*u 即可——丢弃字段无须宽度保真。
    unsigned long long start_jiffies = 0;
    if (std::sscanf(p + 2,
                    "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u "
                    "%*d %*d %*d %*d %*d %*d %llu",
                    &start_jiffies) != 1)
        return false;
    *out = start_jiffies;
    return true;
}
#endif

unsigned long long SelfStartJiffiesCache() {
#if defined(RTDB_HAS_PROCFS)
    unsigned long long j = 0;
    if (ReadProcStartJiffies(getpid(), &j)) return j;
#endif
    return 0;
}

long ClockTicksPerSec() {
#if defined(RTDB_HAS_PROCFS)
    const long hz = ::sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
#else
    return 100;
#endif
}

}  // namespace

uint32_t CurrentProcessId() { return static_cast<uint32_t>(::getpid()); }

uint64_t CurrentProcessStartTimeNs() {
    const unsigned long long j = SelfStartJiffiesCache();
    const long hz = ClockTicksPerSec();
    // jiffies 精度有限（通常 10ms），用作一致性比对键而非真实时间。
    return static_cast<uint64_t>(j) * (1000000000ull / static_cast<uint64_t>(hz));
}

bool IsSameLiveProcess(uint32_t pid, uint64_t start_time_ns) {
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return false;
#if defined(RTDB_HAS_PROCFS)
    unsigned long long j = 0;
    if (!ReadProcStartJiffies(static_cast<pid_t>(pid), &j)) return false;
    const long hz = ClockTicksPerSec();
    return static_cast<uint64_t>(j) * (1000000000ull / static_cast<uint64_t>(hz)) == start_time_ns;
#else
    (void)start_time_ns;
    return true;
#endif
}

std::string HostExecutablePath() {
    char buf[4096];
#if defined(__linux__)
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf, static_cast<size_t>(n));
    }
#endif
    return std::string();
}

}  // namespace platform
}  // namespace rtdb

#endif  // !_WIN32
