#include "infra/failpoint.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rtdb {
namespace failpoint {
namespace detail {

inline std::atomic<bool>& AnyArmed() {
    static std::atomic<bool> v{false};
    return v;
}

inline std::mutex& Mu() {
    static std::mutex m;
    return m;
}

inline std::unordered_map<std::string, Mode>& Map() {
    static std::unordered_map<std::string, Mode> m;
    return m;
}

/// 跨平台读环境变量为 std::string（MSVC 下避开 getenv 的 C4996 弃用告警）。
inline std::string GetEnvCopy(const char* name) {
#if defined(_WIN32)
    char* buf = nullptr;
    size_t len = 0;
    if (::_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return std::string();
    std::string value(buf);
    std::free(buf);
    return value;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

#if defined(__GNUC__) && !defined(__clang__)
extern "C" void __gcov_dump(void) __attribute__((weak));
#endif

/// 硬退出（kill -9 等价）：不析构静态对象、不跑 atexit。
///
/// 与 std::quick_exit 语义一致；区别仅在 gcov 插桩构建（MinGW/UCRT）下：
/// libgcc 覆盖率运行时经退出转储链会触发 UCRT fast-fail(0xC0000409)，
/// 使退出码退化为非预期值、且 .gcda 可能未落盘。故在 weak 符号可用时
/// 先主动 __gcov_dump 再 _Exit——普通构建下 weak 引用为 null，零影响。
[[noreturn]] inline void HardExit(int code) noexcept {
#if defined(__GNUC__) && !defined(__clang__)
    if (__gcov_dump) __gcov_dump();
#endif
    std::fflush(nullptr);
    std::_Exit(code);
}

inline void ArmLocked(const std::string& name, Mode mode) {
    Map()[name] = mode;
    AnyArmed().store(true, std::memory_order_relaxed);
}

/// 解析 "name=mode,name2=mode2" 规格并武装（调用方须持锁）。
inline void ParseSpecInto(const char* spec) {
    if (spec == nullptr || spec[0] == '\0') return;
    const std::string s(spec);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        const std::string item = s.substr(pos, comma - pos);
        pos = comma + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= item.size()) continue;
        const Mode mode = (item.substr(eq + 1) == "error") ? Mode::kError : Mode::kCrash;
        ArmLocked(item.substr(0, eq), mode);
    }
}

inline void ParseEnvOnce() {
    static std::once_flag flag;
    std::call_once(flag, [] { ParseSpecInto(GetEnvCopy("RTDB_FAILPOINTS").c_str()); });
}

/// 命中并解除（返回 true 表示该点以 mode 命中）。
inline bool ConsumeHit(const char* name, Mode mode) {
    std::lock_guard<std::mutex> lk(Mu());
    auto it = Map().find(name);
    if (it == Map().end() || it->second != mode) return false;
    Map().erase(it);
    if (Map().empty()) AnyArmed().store(false, std::memory_order_relaxed);
    return true;
}

}  // namespace detail

void Arm(const char* name, Mode mode) {
    if (name == nullptr) return;
    detail::ParseEnvOnce();
    std::lock_guard<std::mutex> lk(detail::Mu());
    detail::ArmLocked(name, mode);
}

void ClearAll() {
    // 先推进 env 解析（首次调用时武装规格指定的注入点），再整体解除：
    // 崩溃注入子进程依赖此顺序——ClearAll 后 DDL 等前置阶段无注入，
    // 之后 ResetFromEnv 再恢复规格注入，只作用于行写负载。
    detail::ParseEnvOnce();
    std::lock_guard<std::mutex> lk(detail::Mu());
    detail::Map().clear();
    detail::AnyArmed().store(false, std::memory_order_relaxed);
}

void ResetFromEnv() {
    detail::ParseEnvOnce();  // 推进 once 状态，避免后续 API 重复解析
    const std::string env = detail::GetEnvCopy("RTDB_FAILPOINTS");
    std::lock_guard<std::mutex> lk(detail::Mu());
    detail::Map().clear();
    detail::ParseSpecInto(env.c_str());
    detail::AnyArmed().store(!detail::Map().empty(), std::memory_order_relaxed);
}

void CrashPoint(const char* name) {
    detail::ParseEnvOnce();
    if (!detail::AnyArmed().load(std::memory_order_relaxed)) return;
    if (detail::ConsumeHit(name, Mode::kCrash)) detail::HardExit(kCrashExitCode);
}

bool ErrorPoint(const char* name) {
    detail::ParseEnvOnce();
    if (!detail::AnyArmed().load(std::memory_order_relaxed)) return false;
    return detail::ConsumeHit(name, Mode::kError);
}

}  // namespace failpoint
}  // namespace rtdb
