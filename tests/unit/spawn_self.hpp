#ifndef RTDB_TESTS_SPAWN_SELF_HPP_
#define RTDB_TESTS_SPAWN_SELF_HPP_

/// 跨进程测试支撑：以参数自我派生并等待退出（两进程锁互斥/死亡恢复
/// 演示的根基）。Windows 用 CreateProcess，POSIX 用 fork+exec。

#include <string>
#include <vector>

namespace rtdb_test {

struct SpawnOutcome {
    bool spawned = false;
    int exit_code = -1;
};

/// 子进程锁占位崩溃模式标记：main() 见到本参数即进入子逻辑。
inline constexpr const char* kCrashLockFlag = "--rtdb-child-crash-lock";

/// WAL 崩溃注入子进程模式标记：main() 见到本参数即执行脚本化写负载，
/// 由 RTDB_FAILPOINTS 环境变量决定在哪个注入点硬退出。
inline constexpr const char* kWalCrashFlag = "--rtdb-child-wal-crash";

/// 以 [kCrashLockFlag, extra_arg] 启动自身可执行文件并阻塞等待。
SpawnOutcome SpawnSelfWithFlag(const std::string& exe, const std::string& extra_arg);

/// 通用自我派生：以 argv = {exe, args...} 启动自身并阻塞等待退出。
/// 环境变量由父进程原样继承（崩溃注入经 RTDB_FAILPOINTS 传递）。
SpawnOutcome SpawnSelf(const std::string& exe, const std::vector<std::string>& args);

/// 跨平台环境变量设置（子进程注入规格传递用）。
void SetEnvVar(const char* name, const char* value);
void UnsetEnvVar(const char* name);

}  // namespace rtdb_test

#endif  // RTDB_TESTS_SPAWN_SELF_HPP_
