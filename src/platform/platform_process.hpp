#ifndef RTDB_SRC_PLATFORM_PLATFORM_PROCESS_HPP_
#define RTDB_SRC_PLATFORM_PLATFORM_PROCESS_HPP_

#include <cstdint>

#include <string>

#include "rtdb/api_macro.hpp"

namespace rtdb {
namespace platform {

/// 当前进程 PID。
RTDB_API uint32_t CurrentProcessId();

/// 当前进程的唯一化身份标识时间戳（用于共享内存锁的死亡恢复：
/// 单纯 PID 会复用，必须叠加进程启动时间才可靠）。
RTDB_API uint64_t CurrentProcessStartTimeNs();

/// 判定某进程是否仍存活且是"当时那个"进程（PID + 启动时间双匹配，
/// 防 PID 复用误判）。
RTDB_API bool IsSameLiveProcess(uint32_t pid, uint64_t start_time_ns);

/// 本进程可执行文件完整路径（跨进程测试用：自我派生）。
RTDB_API std::string HostExecutablePath();

}  // namespace platform
}  // namespace rtdb

#endif  // RTDB_SRC_PLATFORM_PLATFORM_PROCESS_HPP_
