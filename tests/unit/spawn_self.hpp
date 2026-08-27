#ifndef RTDB_TESTS_SPAWN_SELF_HPP_
#define RTDB_TESTS_SPAWN_SELF_HPP_

/// 跨进程测试支撑：以参数自我派生并等待退出（两进程锁互斥/死亡恢复
/// 演示的根基）。Windows 用 CreateProcess，POSIX 用 fork+exec。

#include <string>

namespace rtdb_test {

struct SpawnOutcome {
  bool spawned = false;
  int exit_code = -1;
};

/// 子进程锁占位崩溃模式标记：main() 见到本参数即进入子逻辑。
inline constexpr const char* kCrashLockFlag = "--rtdb-child-crash-lock";

/// 以 [kCrashLockFlag, extra_arg] 启动自身可执行文件并阻塞等待。
SpawnOutcome SpawnSelfWithFlag(const std::string& exe,
                               const std::string& extra_arg);

}  // namespace rtdb_test

#endif  // RTDB_TESTS_SPAWN_SELF_HPP_
