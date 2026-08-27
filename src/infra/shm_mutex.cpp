#include "infra/shm_mutex.hpp"

#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <chrono>
#include <thread>

#include "platform/platform_process.hpp"

namespace rtdb {
namespace infra {

void ShmRootMutex::InitState(shm_RootMutexState* s) noexcept {
  std::memset(s, 0, sizeof(*s));
}

ShmRootMutex::ShmRootMutex(shm_RootMutexState* s) noexcept
    : s_(s),
      my_pid_(platform::CurrentProcessId()),
      my_start_(platform::CurrentProcessStartTimeNs()) {}

bool ShmRootMutex::TryOnceLocked() noexcept {
  // 快路径：CAS 0 -> 1。
  uint32_t expected = 0;
  if (MutexAtomic(&s_->locked)->compare_exchange_strong(expected, 1)) {
    s_->owner_pid = my_pid_;
    s_->owner_start_ns = my_start_;
    return true;
  }
  if (timeout_no_recover_) return false;  // TryLock 路径不做死亡恢复

  // 慢路径：锁被占 —— 判定持有者是否已死，死了则抢锁恢复。
  const uint32_t dead_pid = s_->owner_pid;
  const uint64_t dead_start = s_->owner_start_ns;
  if (dead_pid == 0 ||
      platform::IsSameLiveProcess(dead_pid, dead_start))
    return false;  // 活着或字段尚未写全，让外层退避后重试

  // CAS 抢占 owner 身份：多恢复者竞争时只有一人成功。
  expected = dead_pid;
  if (MutexAtomic(&s_->owner_pid)->compare_exchange_strong(expected,
                                                           my_pid_)) {
    s_->owner_start_ns = my_start_;  // 接管完成
    return true;
  }
  return false;  // 别的恢复者抢先了，下一轮再评估
}

bool ShmRootMutex::TryLock() noexcept {
  timeout_no_recover_ = true;
  const bool ok = TryOnceLocked();
  timeout_no_recover_ = false;
  return ok;
}

Err ShmRootMutex::Lock(uint32_t timeout_ms) noexcept {
  const auto deadline =
      std::chrono::steady_clock::now() +
      (timeout_ms == 0xFFFFFFFFu
           ? std::chrono::steady_clock::duration::max()
           : std::chrono::milliseconds(timeout_ms));
  for (int spin = 0;; ++spin) {
    if (TryOnceLocked()) return Err::kOk;
    if (std::chrono::steady_clock::now() >= deadline) return Err::kTimeout;
    // 指数退避：先自旋烧 CPU 更省上下文切换，久等则让出核心。
    if (spin < 64) {
      #if defined(_MSC_VER)
      _mm_pause();
      #else
      std::this_thread::yield();
      #endif
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(spin < 1024 ? 50 : 500));
    }
  }
}

void ShmRootMutex::Unlock() noexcept {
  // 先抹掉身份再开门：避免门刚开的一瞬被误读为"有主但身份不全"。
  s_->owner_start_ns = 0;
  MutexAtomic(&s_->owner_pid)->store(0, std::memory_order_release);
  MutexAtomic(&s_->locked)->store(0, std::memory_order_release);
}

}  // namespace infra
}  // namespace rtdb
