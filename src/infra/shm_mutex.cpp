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

using layout::AsAtomicU64;  // 64 位字段的原子访问

// ============================================================
// 原子访问 / 内存序约定（与 shm_mutex.hpp 状态字注释一致）
// ============================================================
// 三个状态字字段在共享内存里的物理类型是整型，但任何并发访问都必须经
// 原子视角：locked / owner_pid 用 MutexAtomic(uint32_t)，owner_start_ns
// 用 AsAtomicU64(uint64_t)。plain 访问与 atomic 访问混用即构成 C++ 数据
// 竞争（tsan-linux 作业 MutexEnv.ThreadedContentionSerializes /
// SubscribeTest.ConcurrentWriterNoLossNoDuplicate 即由 Unlock 的 plain
// 清零与 TryOnceLocked 的 atomic 读混用触发）。
//
// 身份发布协议（owner_start_ns 是载荷，owner_pid 是"发布完成"标志）：
//   写者：store(owner_start_ns, relaxed) → store(owner_pid, release)
//   读者：load(owner_pid, acquire)       → load(owner_start_ns, relaxed)
// 读者观察到 pid != 0 时，release/acquire 配对保证随后读到的 start 属于
// 同一次发布；故 start 侧 relaxed 已足够，无需额外栅栏。
//
// 互斥门闩（locked）：
//   加锁 CAS 成功用 acquire —— 与上一持有者 Unlock 中 locked 的 release
//   存储配对，使其临界区内的普通共享内存写对本次持有可见（这正是锁的
//   可见性语义；原先的 seq_cst 是更强而非必要的排序，降为 acquire 不改变
//   任何被依赖的保证）。
//   解锁用 release —— 发布本临界区写；身份清零（relaxed/release）都排在该
//   release 存储之前，故新持有者 acquire 到 locked==0 时必然也看到身份已空。
//   加锁成功后本持有者写的身份由后续 Unlock 的 release 统一发布，故 CAS
//   成功侧不需要 release。

void ShmRootMutex::InitState(shm_RootMutexState* s) noexcept {
    // 唯一允许的 plain 访问：只在映像首次创建、状态字尚未对任何其他
    // 线程/进程发布前调用（shm_region.cpp Open 的 header_all_zero 分支）。
    std::memset(s, 0, sizeof(*s));
}

ShmRootMutex::ShmRootMutex(shm_RootMutexState* s) noexcept
    : s_(s),
      my_pid_(platform::CurrentProcessId()),
      my_start_(platform::CurrentProcessStartTimeNs()) {}

bool ShmRootMutex::TryOnceLocked() noexcept {
    // 快路径：CAS 0 -> 1（acquire 侧见文件头"互斥门闩"）。
    // 属主身份必须"先 start 后 pid"发布（见文件头"身份发布协议"）。
    uint32_t expected = 0;
    if (MutexAtomic(&s_->locked)
            ->compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
        AsAtomicU64(&s_->owner_start_ns)->store(my_start_, std::memory_order_relaxed);
        MutexAtomic(&s_->owner_pid)->store(my_pid_, std::memory_order_release);
        return true;
    }
    if (timeout_no_recover_) return false;  // TryLock 路径不做死亡恢复

    // 慢路径：锁被占 —— 判定持有者是否已死，死了则抢锁恢复。
    // 发布协议：pid 是发布完成的标志（acquire 读取）。pid==0 或
    // start==0 均视为"发布中/未知"，绝不据此判定死亡——否则会在
    // 新持有者尚未写全身份的窗口内发生假死偷锁，造成临界区重叠。
    const uint32_t dead_pid = MutexAtomic(&s_->owner_pid)->load(std::memory_order_acquire);
    if (dead_pid == 0) return false;
    const uint64_t dead_start = AsAtomicU64(&s_->owner_start_ns)->load(std::memory_order_relaxed);
    if (dead_start == 0) return false;
    if (platform::IsSameLiveProcess(dead_pid, dead_start)) return false;  // 活着

    // CAS 抢占 owner 身份：多恢复者竞争时只有一人成功。
    // acq_rel：acquire 半部与死亡持有者 pid 的 release 存储配对（看到其
    // 已发布身份）；release 半部把"本轮接管"发布给其他恢复者（其下一轮
    // acquire 读到我的 pid 时可见我此前的写）。
    expected = dead_pid;
    if (MutexAtomic(&s_->owner_pid)
            ->compare_exchange_strong(expected, my_pid_, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        // 注意：pid 已由上面的 CAS 发布，start 在此随后写入。两者都是原子
        // 访问（无 UB、无 TSan 数据竞争），但存在极窄的
        // "(my_pid, 旧 start)" 观察窗口：跨进程双恢复者竞争中，另一恢复者
        // 在该窗口内读到新 pid + 旧 start 会误判"已死"并再抢一次。这是
        // "两字身份 + 单字发布标志"协议的固有局限（彻底修复需把身份发布
        // 收敛到单个原子字，涉及协议/布局变更），已列入已知残余风险。
        AsAtomicU64(&s_->owner_start_ns)->store(my_start_, std::memory_order_relaxed);
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
    const auto deadline = std::chrono::steady_clock::now() +
                          (timeout_ms == 0xFFFFFFFFu ? std::chrono::steady_clock::duration::max()
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
    // 三个存储都是原子访问（此前 owner_start_ns 是 plain 写，与
    // TryOnceLocked 的 atomic 读混用 → TSan 数据竞争，本次修复）。
    // 顺序：身份清零（relaxed start / release pid）都排在 locked 的
    // release 存储之前 —— release 语义覆盖本线程此前全部写，故下一个
    // acquire 到 locked==0 的持有者必然看到身份已清空与本临界区写。
    AsAtomicU64(&s_->owner_start_ns)->store(0, std::memory_order_relaxed);
    MutexAtomic(&s_->owner_pid)->store(0, std::memory_order_release);
    MutexAtomic(&s_->locked)->store(0, std::memory_order_release);
}

}  // namespace infra
}  // namespace rtdb
