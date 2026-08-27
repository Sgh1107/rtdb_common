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

using namespace layout;  // 分区常量

/// 共享内存根互斥锁状态字（位于固定偏移 kRootMutexOffset，
/// 所有字段宽度天然原子，无需 padding 对齐技巧）。
struct shm_RootMutexState {
  uint32_t locked;          ///< 0=空闲 1=持有（AsAtomicU32 访问）
  uint32_t owner_pid;       ///< 持有者 PID（0=无）
  uint64_t owner_start_ns;  ///< 持有者启动时间（防 PID 复用）
};
static_assert(sizeof(shm_RootMutexState) == 16, "状态字须恰好 16 字节");

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
/// 注意：本锁不可重入；持锁时间必须短（无系统调用/长循环）。
class RTDB_API ShmRootMutex {
 public:
  /// 首次创建时清零状态字。
  static void InitState(shm_RootMutexState* s) noexcept;

  explicit ShmRootMutex(
      shm_RootMutexState* s) noexcept;  // 假定已被 InitState 初始化

  /// 加锁。timeout_ms 为 0 等价 TryLock（不参与死亡恢复）；
  /// 死亡恢复最多消耗一次活性探测的时间。返回 kTimeout 表示超时。
  Err Lock(uint32_t timeout_ms) noexcept;
  /// 纯尝试：只看是否空闲，不做死亡恢复。
  bool TryLock() noexcept;
  /// 解锁。仅允许当前持有者调用。
  void Unlock() noexcept;

  bool IsLockedBySelf() const noexcept {
    return s_->locked == 1 && s_->owner_pid == my_pid_ &&
           s_->owner_start_ns == my_start_;
  }

 private:
  bool TryOnceLocked() noexcept;  ///< 尝试一次获取（含死亡恢复）

  shm_RootMutexState* s_;
  uint32_t my_pid_;
  uint64_t my_start_;
  bool timeout_no_recover_ = false;  ///< TryLock 置位：禁止死亡恢复
};

}  // namespace infra
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_SHM_MUTEX_HPP_
