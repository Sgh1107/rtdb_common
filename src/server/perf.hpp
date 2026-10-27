#ifndef RTDB_SRC_SERVER_PERF_HPP_
#define RTDB_SRC_SERVER_PERF_HPP_

/// \file
/// \brief M4 PerfCounters（docs/16 D3）：host 内存的进程内原子计数。
///
/// 设计：所有计数为 `std::atomic<uint64_t>`（relaxed 递增即可，快照允许
/// 轻微不一致），热路径只做一次原子加，无锁、无分配。指标口径写入 C4
/// 运维手册；对外经 `RTDB_PF_ADMIN/STATS`（版本化 TLV）暴露，不改变 shm
/// 布局（跨进程固定计数块顺延 M5）。
///
/// 说明：本结构只承载"服务端自身面"计数；引擎水位（committed/checkpoint/
/// 心跳）与引擎计数在 STATS 应答时从 `Engine` 实时读取。

#include <atomic>
#include <cstdint>

namespace rtdb {
namespace server {

struct PerfCounters {
    std::atomic<uint64_t> accepted{0};            ///< 接受连接数（进入会话线程）
    std::atomic<uint64_t> rejected_full{0};       ///< 超会话上限拒绝数（404）
    std::atomic<uint64_t> auth_ok{0};             ///< AUTH 成功次数
    std::atomic<uint64_t> auth_fail{0};           ///< AUTH 失败次数（含限次断连）
    std::atomic<uint64_t> requests{0};            ///< 已处理请求帧数
    std::atomic<uint64_t> frame_errors{0};        ///< 帧级违例/EOF（连接被关闭）
    std::atomic<uint64_t> payload_errors{0};      ///< 业务错误应答数
    std::atomic<uint64_t> authz_denied{0};        ///< 权限拒绝次数（审计计数）
    std::atomic<uint64_t> sessions_current{0};    ///< 当前活跃会话数
    std::atomic<uint64_t> sessions_total{0};      ///< 累计会话数（AUTH 成功会话）
    std::atomic<uint64_t> handshake_timeouts{0};  ///< 未认证握手超时关闭数
    std::atomic<uint64_t> idle_timeouts{0};       ///< 已认证空闲超时关闭数
    std::atomic<uint64_t> auth_limit_closed{0};   ///< AUTH 失败超限断连数
    std::atomic<uint64_t> admin_requests{0};      ///< ADMIN/AUTHZ_ME 请求数
};

}  // namespace server
}  // namespace rtdb

#endif  // RTDB_SRC_SERVER_PERF_HPP_
