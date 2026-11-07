/// \file
/// \brief 控制台输出编码配置（docs/18 §6 排障）。
///
/// Windows 控制台默认按**系统代码页**（简中系统为 GBK/936）解码进程写出的
/// 字节，而本库所有源码、日志与诊断输出均为 UTF-8：中文 Windows 下直接
/// `printf` 中文会显示成乱码（如 `鍚姩澶辫触`）。控制台程序在 `main` 开头
/// 调用 `rtdb::platform::UseUtf8Console()` 把输出代码页切到 UTF-8（65001）
/// 即可正常显示，无需用户手动 `chcp 65001`。
///
/// 边界说明：
///  - 只影响“控制台显示”；重定向到文件/管道的字节流本来就是 UTF-8，不受影响。
///  - 非 Windows 平台（POSIX 终端按 UTF-8 解释）为空操作。
///  - 无控制台（服务、纯重定向）时 `SetConsoleOutputCP` 失败无副作用。
#ifndef RTDB_PLATFORM_CONSOLE_HPP_
#define RTDB_PLATFORM_CONSOLE_HPP_

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rtdb {
namespace platform {

/// 把当前控制台输出代码页切到 UTF-8（65001）；非 Windows / 无控制台为空操作。
/// 幂等，可在进程入口重复调用。
inline void UseUtf8Console() noexcept {
#if defined(_WIN32)
    (void)SetConsoleOutputCP(CP_UTF8);
#endif
}

}  // namespace platform
}  // namespace rtdb

#endif  // RTDB_PLATFORM_CONSOLE_HPP_
