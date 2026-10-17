/// \file Win32 跨进程等待原语实现：WaitOnAddress / WakeByAddressAll
/// （Windows 8+；经 GetProcAddress 动态解析，避免工具链导入库差异；
/// 不可用时回退为 1ms 短睡轮询，语义保持一致）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "platform/ipc_wait.hpp"

namespace rtdb {
namespace platform {

namespace {

using WaitOnAddressFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressAllFn = VOID(WINAPI*)(PVOID);

struct AddressWaitApi {
    WaitOnAddressFn wait = nullptr;
    WakeByAddressAllFn wake = nullptr;
};

/// Windows 8+ 的 kernelbase.dll 导出 WaitOnAddress/WakeByAddressAll；
/// 动态解析一次并复用（失败 = 老系统，走轮询回退）。
const AddressWaitApi& GetAddressWaitApi() noexcept {
    static const AddressWaitApi api = []() {
        AddressWaitApi a;
        HMODULE mod = GetModuleHandleW(L"kernelbase.dll");
        if (mod == nullptr) mod = GetModuleHandleW(L"kernel32.dll");
        if (mod != nullptr) {
            a.wait = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(mod, "WaitOnAddress"));
            a.wake = reinterpret_cast<WakeByAddressAllFn>(GetProcAddress(mod, "WakeByAddressAll"));
            if (a.wait == nullptr || a.wake == nullptr) {
                a.wait = nullptr;
                a.wake = nullptr;
            }
        }
        return a;
    }();
    return api;
}

/// 轮询回退（WaitOnAddress 不可用）：值未变时 1ms 短睡。
bool PollFallback(const std::atomic<uint32_t>* word, uint32_t expect, uint32_t timeout_ms) noexcept {
    const bool infinite = (timeout_ms == kWaitInfiniteMs);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(infinite ? 60000u : timeout_ms);
    for (;;) {
        if (word->load(std::memory_order_acquire) != expect) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

bool Wait32(const uint32_t* addr, uint32_t expect, uint32_t timeout_ms) noexcept {
    if (addr == nullptr) return false;
    const auto* word = reinterpret_cast<const std::atomic<uint32_t>*>(addr);
    if (word->load(std::memory_order_acquire) != expect) return true;
    if (timeout_ms == 0) return false;

    const AddressWaitApi& api = GetAddressWaitApi();
    if (api.wait == nullptr) return PollFallback(word, expect, timeout_ms);

    const DWORD budget = (timeout_ms == kWaitInfiniteMs) ? INFINITE : static_cast<DWORD>(timeout_ms);
    // WaitOnAddress 依据物理页比较/唤醒：同文件映射的跨进程视图共享物理页，
    // 因此对不同虚拟地址的等待与唤醒同样有效（docs/13 §3-D4）。
    const BOOL woken = api.wait(const_cast<volatile uint32_t*>(addr), &expect, sizeof(uint32_t),
                                budget);
    if (woken) return true;
    // 超时之外的异常返回也一律按"可能已变"处理，由调用方复核真实值。
    return GetLastError() != ERROR_TIMEOUT;
}

void WakeAll32(uint32_t* addr) noexcept {
    if (addr == nullptr) return;
    const AddressWaitApi& api = GetAddressWaitApi();
    if (api.wake != nullptr) api.wake(addr);
    // 轮询回退模型无唤醒动作（≤1ms 自醒）。
}

}  // namespace platform
}  // namespace rtdb
