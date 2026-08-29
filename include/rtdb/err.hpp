#ifndef RTDB_ERR_HPP_
#define RTDB_ERR_HPP_

#include <cstdint>

#include "rtdb/api_macro.hpp"

namespace rtdb {

/// 全库统一错误码。分段规划：
///   0    成功
///   1xx  基础设施/平台层
///   2xx  存储层与共享内存布局
///   3xx  引擎层（M1 起启用）
///   4xx  服务端/网络协议
///   5xx  权限
enum class Err : int16_t {
    kOk = 0,
    // ---- 1xx 基础设施 ----
    kInvalidArgument = 101,
    kNotFound = 102,
    kAlreadyExists = 103,
    kIoError = 104,
    kUnsupported = 105,
    kOutOfMemory = 106,
    kTimeout = 107,
    // ---- 2xx 存储/布局 ----
    kBadSuperBlock = 201,
    kVersionMismatch = 202,
    kIncompatibleLayout = 203,
    kReadOnlyHandle = 204,
    kCorruption = 205,
    // ---- 3xx 引擎层（M1 起启用，docs/08 §4）----
    /// 实例毒化：WAL 环写失败/校验失败后拒写保读（docs/08 §2-D2）。
    kPoisoned = 301,
    /// 建表重名（Catalog 目录事务拒绝）。
    kTableExists = 302,
    /// OpenTable/DropTable 目标表不存在。
    kTableNotFound = 303,
    /// schema 不匹配：句柄绑定的布局版本与当前表定义不一致等。
    kSchemaMismatch = 304,
    /// 原子批量被整体作废（未落 COMMIT，恢复时整批丢弃）。
    kBatchAborted = 305,
    /// 超出 SuperBlock 配置上限（表数/列数/索引数）。
    kLimitExceeded = 306,
    // ---- 4xx 服务端 ----
    kNotImplemented = 400,
    kNetworkError = 401,
    // ---- 兜底 ----
    kInternal = 999,
};

inline bool IsOk(Err e) noexcept { return e == Err::kOk; }

/// 返回人类可读的简短描述；未知码返回 generic 文案。线程安全。
RTDB_API const char* ErrorMessage(Err e) noexcept;

}  // namespace rtdb

#endif  // RTDB_ERR_HPP_
