#ifndef RTDB_SRC_INFRA_SHM_MUTEX_HPP_
#define RTDB_SRC_INFRA_SHM_MUTEX_HPP_

#include <atomic>
#include <cstdint>
#include <cstring>

#include "infra/layout.hpp"
#include "rtdb/api_macro.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"

namespace rtdb {
namespace infra {

/// 共享内存根互斥锁状态字（位于固定偏移 kRootMutexOffset —— 见
/// infra/layout.hpp 的分区常量；所有字段宽度天然原子，无需对齐技巧）。
///
/// 并发访问约定（C++17 无 atomic_ref，沿用 layout.hpp 的
/// "reinterpret_cast 到等宽 atomic" 惯用法；本头文件 + shm_mutex.cpp
/// 是该状态字的唯一访问点）：
///   - 三个字段在共享内存里的物理类型是整型，但**一切并发访问必须经
///     原子视角**：locked / owner_pid 走 MutexAtomic()，
///     owner_start_ns 走 layout::AsAtomicU64()。plain 读写与另一侧的
///     atomic 访问混用即构成 C++ 数据竞争（tsan-linux 作业已实测命中
///     Unlock 的 plain 清零 vs TryOnceLocked 的 atomic 读）。
///   - 唯一例外：InitState() 的 memset —— 只允许在映像首次创建、状态字
///     尚未对任何其他线程/进程发布前调用（shm_region.cpp Open 的
///     header_all_zero 分支）。
///
/// 身份发布协议（owner_start_ns 是载荷、owner_pid 是"发布完成"标志）：
///   写者：store(owner_start_ns, relaxed) → store(owner_pid, release)
///   读者：load(owner_pid, acquire)       → load(owner_start_ns, relaxed)
/// 读者若观察到 owner_pid != 0，release/acquire 配对保证随后读到的
/// owner_start_ns 属于同一次发布；因此 start 一侧 relaxed 已足够。
struct shm_RootMutexState {
    uint32_t locked;          ///< 0=空闲 1=持有（原子访问：MutexAtomic）
    uint32_t owner_pid;       ///< 持有者 PID，0=无（原子；身份发布完成标志）
    uint64_t owner_start_ns;  ///< 持有者启动时间，防 PID 复用（原子：AsAtomicU64）
};
static_assert(sizeof(shm_RootMutexState) == 16, "状态字须恰好 16 字节");

/// uint32_t 状态字的原子视角（唯一访问入口，禁止对同一字再做 plain 读写）。
inline std::atomic<uint32_t>* MutexAtomic(uint32_t* p) noexcept {
    return reinterpret_cast<std::atomic<uint32_t>*>(p);
}

/// 根互斥锁（保护分配器/BTree 目录等全局变更；粒度=整实例粗锁）。
///
/// 持有者崩溃后锁残留：Lock() 路径会检测持有者存活状况（PID+启动
/// 时间双匹配），确认死亡后"抢锁恢复"。正确性论证：
///   - 存活判定假阴性不可能引起数据竞争：若误判死亡并偷锁，原持有
///     者实际仍活着——由"启动时间匹配"排除同 PID 异进程；持有期间
///     进程必然存活，故只在真崩溃时进入恢复分支。
///   - 多个恢复者并发偷锁：owner_pid 单字 CAS 决出唯一赢家，输家下
///     一轮发现新持有者存活即退出。
///   - 状态字三字段一律经原子视角访问（见 shm_RootMutexState 注释）；
///     加锁 acquire / 解锁 release 配对，身份按 start→pid 顺序发布。
///     任何 plain 读写都会与另一侧 atomic 访问构成数据竞争（TSan 会报）。
/// 注意：本锁不可重入；持锁时间必须短（无系统调用/长循环）。
class RTDB_API ShmRootMutex {
public:
    /// 首次创建时清零状态字。
    static void InitState(shm_RootMutexState* s) noexcept;

    explicit ShmRootMutex(shm_RootMutexState* s) noexcept;  // 假定已被 InitState 初始化

    /// 加锁。timeout_ms 为 0 等价 TryLock（不参与死亡恢复）；
    /// 死亡恢复最多消耗一次活性探测的时间。返回 kTimeout 表示超时。
    Err Lock(uint32_t timeout_ms) noexcept;
    /// 纯尝试：只看是否空闲，不做死亡恢复。
    bool TryLock() noexcept;
    /// 解锁。仅允许当前持有者调用。
    void Unlock() noexcept;

    /// \brief 当前实例（线程/进程身份）是否仍持有锁。
    ///
    /// 只读观测，全部字段经原子视角读取，与加解锁的原子写保持同一访问
    /// 类型（混用 atomic/plain 即数据竞争）。读取顺序即发布协议的读侧：
    /// 先 acquire locked 确认"持有"，再 acquire pid 确认身份发布完成，
    /// 最后 relaxed 读 start 载荷（acquire(pid) 已保证其可见性）。
    bool IsLockedBySelf() const noexcept {
        if (MutexAtomic(&s_->locked)->load(std::memory_order_acquire) != 1) return false;
        if (MutexAtomic(&s_->owner_pid)->load(std::memory_order_acquire) != my_pid_) return false;
        return layout::AsAtomicU64(&s_->owner_start_ns)->load(std::memory_order_relaxed) ==
               my_start_;
    }

private:
    bool TryOnceLocked() noexcept;  ///< 尝试一次获取（含死亡恢复）

    shm_RootMutexState* s_;
    uint32_t my_pid_;
    uint64_t my_start_;
    bool timeout_no_recover_ = false;  ///< TryLock 置位：禁止死亡恢复
};

/// \brief 根互斥锁的 RAII 守卫。
///
/// 构造时按给定超时加锁，析构时仅在成功持有时解锁；用于引擎内部
/// 临界区（目录事务、行写入、分配器操作），避免异常/早退路径漏解锁。
///
/// \note 锁不可重入：同一作用域内不得嵌套构造同一个 `ShmRootMutex` 的守卫。
class ShmRootMutexGuard {
public:
    /// \brief 构造并加锁。
    /// \param mutex 目标锁（nullptr 表示无锁可用，如 Reader 角色）
    /// \param timeout_ms 加锁超时（毫秒；0xFFFFFFFF 表示无限等待）
    ShmRootMutexGuard(ShmRootMutex* mutex, uint32_t timeout_ms) noexcept
        : mutex_(mutex), locked_(false), err_(Err::kUnsupported) {
        if (mutex_ != nullptr) {
            err_ = mutex_->Lock(timeout_ms);
            locked_ = IsOk(err_);
        }
    }

    ~ShmRootMutexGuard() noexcept {
        if (locked_) mutex_->Unlock();
    }

    ShmRootMutexGuard(const ShmRootMutexGuard&) = delete;
    ShmRootMutexGuard& operator=(const ShmRootMutexGuard&) = delete;

    /// \brief 是否成功持有锁。
    bool locked() const noexcept { return locked_; }

    /// \brief 加锁结果（未持有时为失败码）。
    Err error() const noexcept { return err_; }

private:
    ShmRootMutex* mutex_;
    bool locked_;
    Err err_;
};

}  // namespace infra
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_SHM_MUTEX_HPP_
