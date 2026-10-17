/// \file W1 跨进程等待原语单测（docs/13 §5-W1）：唤醒/超时/零超时轮询。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "platform/ipc_wait.hpp"

namespace {

using rtdb::platform::Wait32;
using rtdb::platform::WakeAll32;

std::uint32_t* WordOf(std::atomic<uint32_t>* w) noexcept {
    return reinterpret_cast<std::uint32_t*>(w);
}

const std::uint32_t* WordOf(const std::atomic<uint32_t>* w) noexcept {
    return reinterpret_cast<const std::uint32_t*>(w);
}

TEST(IpcWaitTest, ImmediateMismatchReturnsFast) {
    std::atomic<uint32_t> word{7};
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(Wait32(WordOf(&word), 3, 5000));  // 值已不等 → 立即返回
    const auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count(), 1000);
}

TEST(IpcWaitTest, WakesOnChange) {
    std::atomic<uint32_t> word{0};
    std::thread changer([&word]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        word.store(1, std::memory_order_release);
        WakeAll32(WordOf(&word));
    });
    EXPECT_TRUE(Wait32(WordOf(&word), 0, 5000));
    EXPECT_EQ(word.load(), 1u);
    changer.join();
}

TEST(IpcWaitTest, TimesOut) {
    std::atomic<uint32_t> word{5};
    EXPECT_FALSE(Wait32(WordOf(&word), 5, 30));  // 值未变 → 超时
    EXPECT_EQ(word.load(), 5u);
}

TEST(IpcWaitTest, ZeroTimeoutIsPoll) {
    std::atomic<uint32_t> word{1};
    EXPECT_TRUE(Wait32(WordOf(&word), 0, 0));   // 值不等 → 立即 true
    EXPECT_FALSE(Wait32(WordOf(&word), 1, 0));  // 值相等且零超时 → false
    WakeAll32(WordOf(&word));                   // 无等待者：无副作用
}

TEST(IpcWaitTest, WakeBeforeWaitIsSafe) {
    // 先唤醒后等待：等待时值已变 → 立即返回（无丢失唤醒语义）。
    std::atomic<uint32_t> word{0};
    word.store(9, std::memory_order_release);
    WakeAll32(WordOf(&word));
    EXPECT_TRUE(Wait32(WordOf(&word), 0, 1000));
}

}  // namespace
