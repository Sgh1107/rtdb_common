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

/// 客户端库（rtdb_client 动态库，M3-W4）符号导出宏：构建库时由 CMake
/// 注入 RTDB_CLIENT_BUILDING_DLL；静态使用（覆盖率构建）注入
/// RTDB_CLIENT_STATIC；消费方默认 dllimport。与 RTDB_API 相互独立
/// （rtdbcore 与 rtdb_client 是两个发行物）。
#if defined(_WIN32)
#if defined(RTDB_CLIENT_BUILDING_DLL)
#define RTDB_CLIENT_API __declspec(dllexport)
#elif defined(RTDB_CLIENT_STATIC)
#define RTDB_CLIENT_API
#else
#define RTDB_CLIENT_API __declspec(dllimport)
#endif
#else
#define RTDB_CLIENT_API __attribute__((visibility("default")))
#endif

#endif  // RTDB_API_MACRO_HPP_
