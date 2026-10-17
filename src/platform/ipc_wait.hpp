#ifndef RTDB_SRC_PLATFORM_IPC_WAIT_HPP_
#define RTDB_SRC_PLATFORM_IPC_WAIT_HPP_

/// \file
/// \brief 跨进程 32 位等待字原语（docs/13 §3-D4）：futex / WaitOnAddress。
///
/// 语义与 futex 一致：值 != expect 时立即返回；否则阻塞至被唤醒或超时。
/// **唤醒语义仅为加速**：调用方在返回后必须复核真实值并自行循环——
/// 值变化与唤醒之间的任意交错都不会导致丢失（等待时校验期望值，
/// 超时兜底），因此无须处理"唤醒丢失"。
///
/// 典型用法（ChangeCursor::Next / FETCH_DELTA 长轮询）：
/// ```cpp
/// const uint64_t committed = AsAtomic(&sb->committed_seq)->load();
/// if (committed <= cursor) {
///     Wait32(reinterpret_cast<const uint32_t*>(&sb->committed_seq),
///            static_cast<uint32_t>(committed), remaining_ms);
/// }
/// ```

#include <cstdint>

#include "rtdb/api_macro.hpp"

namespace rtdb {
namespace platform {

/// 无限等待哨兵（与 net.hpp kInfiniteTimeoutMs 同值）。
inline constexpr uint32_t kWaitInfiniteMs = 0xFFFFFFFFu;

/// 等待 `*addr` 的值相对 `expect` 发生变化。
/// \return true = 可能已被唤醒/值已变（**必须复核真实值**）；
///         false = 超时（等待期间未观察到变化；唤醒竞态下仍以复核为准）。
RTDB_API bool Wait32(const uint32_t* addr, uint32_t expect, uint32_t timeout_ms) noexcept;

/// 唤醒全部等待者。对无等待者的地址无副作用（允许在任何时刻调用）。
RTDB_API void WakeAll32(uint32_t* addr) noexcept;

}  // namespace platform
}  // namespace rtdb

#endif  // RTDB_SRC_PLATFORM_IPC_WAIT_HPP_
