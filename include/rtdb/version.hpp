#ifndef RTDB_VERSION_HPP_
#define RTDB_VERSION_HPP_

#include <cstdint>

#define RTDB_VERSION_MAJOR 0
#define RTDB_VERSION_MINOR 1
#define RTDB_VERSION_PATCH 0
/// C ABI 语义版本（docs/04 §1：CAbi 符号冻结语义版本化）。
/// v2 = M3 订阅/会话符号集（与 rtdb_cabi.h 的 RTDB_ABI_VER 必须一致；
/// src/api/c_abi.cpp 内 static_assert 做编译期校验）。
#define RTDB_ABI_VERSION 2

namespace rtdb {

inline constexpr const char* kVersionString = "0.1.0";

/// 共享内存布局版本：任何会破坏存量映像可读性的布局改动必须递增此值，
/// 并在迁移工具中提供升级路径。
/// v2（2026-08，M1-W1）：SuperBlock 增补 WalRing/目录/上限规划字段与
/// checkpoint_seq/flushed_seq 运行时字段（docs/08 §3-W1）。
inline constexpr uint32_t kShmFormatVersion = 2;

}  // namespace rtdb

#endif  // RTDB_VERSION_HPP_
