#ifndef RTDB_SRC_SERVER_SERVER_HPP_
#define RTDB_SRC_SERVER_SERVER_HPP_

/// \file
/// \brief rtdbd 服务端核心（docs/13 §5-W3）：引擎 + 监听 + 会话/订阅线程。
///
/// 传输模型（docs/13 §3-D3）：阻塞 socket + 线程/连接；会话循环对
/// RECV 采用 250ms 轮询超时，Stop 收敛预算 ≤ 约 0.5s。
/// 本类不含信号处理与 CLI（见 `main.cpp`）；同进程集成测试直接持有
/// 本对象并可用 `engine()` 做写入侧驱动。

#include <cstdint>
#include <memory>
#include <string>

#include "rtdb/engine.hpp"
#include "rtdb/err.hpp"
#include "server/config.hpp"

namespace rtdb {
namespace server {

class Server {
public:
    Server() noexcept;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// \brief 启动：打开引擎（写侧）→ 绑定监听 → 起接受/心跳线程。
    /// 失败时本次已获取资源全部回滚；重复 Start（未 Stop）返回
    /// `kInvalidArgument`。
    Err Start(const Config& cfg);

    /// \brief 优雅退出：停接受 → 等会话线程收敛 →（可选）冷快照 →
    /// 关闭引擎（最终 flush）。幂等；`timeout_ms` 为会话收敛预算。
    void Stop(uint32_t timeout_ms = 30000);

    bool running() const noexcept;

    /// 实际监听端口（配置 0 时取系统分配值；未运行 = 0）。
    uint16_t port() const noexcept;

    /// 引擎句柄（生命周期 = Start..Stop；仅同进程测试/诊断用）。
    Engine* engine() noexcept;

    /// 诊断计数（累计值，单调递增；M4 PerfCounters 的服务端门面）。
    struct Stats {
        uint64_t accepted = 0;        ///< 接受连接数（进入会话循环）
        uint64_t rejected_full = 0;   ///< 超会话上限拒绝数（404 应答后关闭）
        uint64_t auth_ok = 0;
        uint64_t auth_fail = 0;
        uint64_t requests = 0;        ///< 已处理请求帧数
        uint64_t frame_errors = 0;    ///< 帧级违例（连接被关闭）
        uint64_t payload_errors = 0;  ///< 载荷级违例/业务错误应答数
        // ---- M4（ADMIN/STATS 同源；docs/16 D3）----
        uint64_t authz_denied = 0;        ///< 权限拒绝次数（审计）
        uint64_t sessions_current = 0;    ///< 当前活跃会话数
        uint64_t sessions_total = 0;      ///< 累计会话数
        uint64_t handshake_timeouts = 0;  ///< 握手超时关闭数
        uint64_t idle_timeouts = 0;       ///< 空闲超时关闭数
        uint64_t auth_limit_closed = 0;   ///< AUTH 失败超限断连数
        uint64_t admin_requests = 0;      ///< ADMIN/AUTHZ_ME 请求数
    };
    Stats stats() const noexcept;

    /// 最近一次内部错误文本（诊断；空 = 无）。
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace server
}  // namespace rtdb

#endif  // RTDB_SRC_SERVER_SERVER_HPP_
