#ifndef RTDB_API_MACRO_HPP_
#define RTDB_API_MACRO_HPP_

/// 跨平台符号导出宏。构建 librtdbcore 动态库时由 CMake 注入
/// RTDB_CORE_BUILDING_DLL；使用方链接后自动变为 dllimport。
#if defined(_WIN32)
#if defined(RTDB_CORE_BUILDING_DLL)
#define RTDB_API __declspec(dllexport)
#else
#define RTDB_API __declspec(dllimport)
#endif
#else
#define RTDB_API __attribute__((visibility("default")))
#endif

#endif  // RTDB_API_MACRO_HPP_
