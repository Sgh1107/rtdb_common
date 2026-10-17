/// \file POSIX 跨进程等待原语实现：Linux futex（非 Linux 为非目标平台，
/// 短睡轮询降级；语义保持一致）。

#include "platform/ipc_wait.hpp"

#include <atomic>

#ifdef __linux__

#include <cerrno>
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace rtdb {
namespace platform {

bool Wait32(const uint32_t* addr, uint32_t expect, uint32_t timeout_ms) noexcept {
    if (addr == nullptr) return false;
    const auto* word = reinterpret_cast<const std::atomic<uint32_t>*>(addr);
    if (word->load(std::memory_order_acquire) != expect) return true;
    if (timeout_ms == 0) return false;

    timespec ts{};
    timespec* pts = nullptr;
    if (timeout_ms != kWaitInfiniteMs) {
        ts.tv_sec = static_cast<time_t>(timeout_ms / 1000u);
        ts.tv_nsec = static_cast<long>(timeout_ms % 1000u) * 1000000L;
        pts = &ts;
    }
    // 跨进程共享内存等待：必须用非 PRIVATE 的 FUTEX_WAIT（同一物理页生效）。
    const long rc = syscall(SYS_futex, reinterpret_cast<int32_t*>(const_cast<uint32_t*>(addr)),
                            FUTEX_WAIT, static_cast<int32_t>(expect), pts, nullptr, 0);
    if (rc == 0) return true;
    // EAGAIN=值已变；EINTR=信号打断——均按"可能已变"处理，超时仅 ETIMEDOUT。
    return errno != ETIMEDOUT;
}

void WakeAll32(uint32_t* addr) noexcept {
    if (addr == nullptr) return;
    syscall(SYS_futex, reinterpret_cast<int32_t*>(addr), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

}  // namespace platform
}  // namespace rtdb

#else  // 非 Linux POSIX：轮询降级

#include <chrono>
#include <thread>

namespace rtdb {
namespace platform {

bool Wait32(const uint32_t* addr, uint32_t expect, uint32_t timeout_ms) noexcept {
    if (addr == nullptr) return false;
    const auto* word = reinterpret_cast<const std::atomic<uint32_t>*>(addr);
    const bool infinite = (timeout_ms == kWaitInfiniteMs);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(infinite ? 60000u : timeout_ms);
    for (;;) {
        if (word->load(std::memory_order_acquire) != expect) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void WakeAll32(uint32_t* addr) noexcept {
    (void)addr;  // 轮询模型无唤醒动作
}

}  // namespace platform
}  // namespace rtdb

#endif  // __linux__
