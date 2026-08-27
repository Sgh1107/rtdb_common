#ifndef RTDB_VERSION_HPP_
#define RTDB_VERSION_HPP_

#include <cstdint>

#define RTDB_VERSION_MAJOR 0
#define RTDB_VERSION_MINOR 1
#define RTDB_VERSION_PATCH 0
#define RTDB_ABI_VERSION 0

namespace rtdb {

inline constexpr const char* kVersionString = "0.1.0-m0";

/// 共享内存布局版本：任何会破坏存量映像可读性的布局改动必须递增此值，
/// 并在迁移工具中提供升级路径。
inline constexpr uint32_t kShmFormatVersion = 1;

}  // namespace rtdb

#endif  // RTDB_VERSION_HPP_
