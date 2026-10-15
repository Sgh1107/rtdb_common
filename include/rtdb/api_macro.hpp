#ifndef RTDB_API_MACRO_HPP_
#define RTDB_API_MACRO_HPP_

/// 跨平台符号导出宏。构建 librtdbcore 动态库时由 CMake 注入
/// RTDB_CORE_BUILDING_DLL；使用方链接后自动变为 dllimport。静态链接
/// （覆盖率构建，MinGW gcov 数据仅在单模块内可完整转储）注入
/// RTDB_CORE_STATIC，宏为空。
#if defined(_WIN32)
#if defined(RTDB_CORE_BUILDING_DLL)
#define RTDB_API __declspec(dllexport)
#elif defined(RTDB_CORE_STATIC)
#define RTDB_API
#else
#define RTDB_API __declspec(dllimport)
#endif
#else
#define RTDB_API __attribute__((visibility("default")))
#endif

#endif  // RTDB_API_MACRO_HPP_
