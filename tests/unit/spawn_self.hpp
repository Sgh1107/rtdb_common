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

/// W5 冷快照崩溃注入子进程模式标记：main() 见到本参数即执行脚本化负载
/// （建表 + 3 行插入 + CreateSnapshot），由 RTDB_FAILPOINTS 决定在哪个
/// 注入点（snapshot.before_copy）硬退出。
inline constexpr const char* kW5SnapFlag = "--rtdb-child-w5-snap";

/// W5 在线加列崩溃注入子进程模式标记：main() 见到本参数即执行脚本化负载
/// （建表 + 3 行插入 + AlterTableAddColumn），由 RTDB_FAILPOINTS 决定在
/// 哪个注入点（alter.before_link）硬退出。
inline constexpr const char* kW5AlterFlag = "--rtdb-child-w5-alter";

/// W5 二级索引崩溃注入子进程模式标记：main() 见到本参数即执行脚本化负载
/// （建表 + 3 行插入 + CreateSecondaryIndex），由 RTDB_FAILPOINTS 决定在
/// 哪个注入点（secidx.before_link）硬退出。
inline constexpr const char* kW5SecIdxFlag = "--rtdb-child-w5-secidx";

/// 以 [kCrashLockFlag, extra_arg] 启动自身可执行文件并阻塞等待。
SpawnOutcome SpawnSelfWithFlag(const std::string& exe, const std::string& extra_arg);

/// 通用自我派生：以 argv = {exe, args...} 启动自身并阻塞等待退出。
/// 环境变量由父进程原样继承（崩溃注入经 RTDB_FAILPOINTS 传递）。
SpawnOutcome SpawnSelf(const std::string& exe, const std::vector<std::string>& args);

/// \brief 异步子进程句柄（M3-W5 网络混沌：运行中硬杀 kill -9）。
///
/// `Spawn` 立即返回（不等待）；`Kill9` 等价 kill -9 / TerminateProcess；
/// `Wait` 阻塞回收。move-only；析构时若已启动未 Wait 则 Kill9 + Wait
/// （防孤儿进程）。
class ChildProcess {
public:
    ChildProcess() noexcept = default;
    ~ChildProcess();
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// 启动 [exe, args...]（不等待）。
    static ChildProcess Spawn(const std::string& exe, const std::vector<std::string>& args);

    bool spawned() const noexcept { return spawned_; }

    /// 进程是否仍在运行（非阻塞探测）。
    bool alive() noexcept;

    /// 硬杀（幂等；未启动为空操作）。
    void Kill9() noexcept;

    /// 阻塞等待退出并回收（幂等；返回退出码，未启动返回 -1）。
    int Wait() noexcept;

private:
    bool spawned_ = false;
    bool reaped_ = false;
#if defined(_WIN32)
    void* handle_ = nullptr;  ///< HANDLE
#else
    long long pid_ = -1;  ///< pid_t
#endif
};

/// 跨平台环境变量设置（子进程注入规格传递用）。
void SetEnvVar(const char* name, const char* value);
void UnsetEnvVar(const char* name);

}  // namespace rtdb_test

#endif  // RTDB_TESTS_SPAWN_SELF_HPP_
