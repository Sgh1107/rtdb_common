#ifndef RTDB_SRC_PLATFORM_NET_HPP_
#define RTDB_SRC_PLATFORM_NET_HPP_

/// \file
/// \brief 平台网络传输抽象（docs/13 §3-D3）：TCP 连接/监听/收发。
///
/// M3 采用阻塞 socket + socket 级超时（线程/连接模型）；接口保持与具体
/// I/O 模型无关，后续可替换为 IOCP/epoll 反应堆实现而不动上层。
/// 依赖方向：仅 server/client 侧使用；engine 及以下不引用本头。

#include <cstddef>
#include <cstdint>
#include <string>

#include "rtdb/err.hpp"
#include "rtdb/result.hpp"

namespace rtdb {
namespace platform {

/// 无限等待哨兵（Accept / Wait32 的 timeout_ms 取此值）。
inline constexpr uint32_t kInfiniteTimeoutMs = 0xFFFFFFFFu;

/// \brief 网络栈初始化（幂等；Win32 = WSAStartup）。
/// 未初始化或初始化失败时，各原语返回 kNetworkError。
Err NetStartup() noexcept;

/// \brief TCP 客户端/会话端套接字（move-only RAII）。
class TcpSocket {
public:
    TcpSocket() noexcept = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /// 连接（支持主机名/IPv4/IPv6；超时覆盖 DNS 后逐一候选地址的连接尝试）。
    static Result<TcpSocket> Connect(const std::string& host, uint16_t port,
                                     uint32_t timeout_ms) noexcept;

    bool valid() const noexcept;

    /// 设置收发超时（毫秒；0 = 阻塞到底；kInfiniteTimeoutMs 归一为 0）。
    /// 超时先例：Send/Recv 超出预算返回 kTimeout（流可能停在半帧，
    /// 调用方应关闭连接）。
    void SetTimeout(uint32_t timeout_ms) noexcept;

    /// 尽力发送全部字节；对端关闭/网络错 → kNetworkError。
    Err SendAll(const void* data, size_t len) noexcept;

    /// 精确接收 len 字节；对端关闭/EOF → kNetworkError，超时 → kTimeout。
    Err RecvExact(void* buf, size_t len) noexcept;

    /// 双向关闭（唤醒阻塞中的收发线程；句柄仍占用直至 Close/析构）。
    void Shutdown() noexcept;

    /// 释放句柄（幂等）。
    void Close() noexcept;

private:
    friend class TcpListener;  // 接受连接时构造已连接套接字

    uintptr_t handle_ = static_cast<uintptr_t>(-1);
    uint32_t timeout_ms_ = 0;
};

/// \brief TCP 监听器（move-only RAII）。
class TcpListener {
public:
    TcpListener() noexcept = default;
    ~TcpListener();
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// 监听；`bind_addr` 为空 = 仅回环（安全默认，docs/13 §7），
    /// "0.0.0.0"/"::" 显式暴露全网。port=0 时由系统分配（port() 取回）。
    static Result<TcpListener> Listen(const std::string& bind_addr, uint16_t port,
                                      int backlog = 16) noexcept;

    /// 接受一个连接；timeout_ms=0 立即轮询，kInfiniteTimeoutMs 阻塞等待。
    Result<TcpSocket> Accept(uint32_t timeout_ms) noexcept;

    uint16_t port() const noexcept { return port_; }
    bool valid() const noexcept;
    void Close() noexcept;

private:
    uintptr_t handle_ = static_cast<uintptr_t>(-1);
    uint16_t port_ = 0;
};

}  // namespace platform
}  // namespace rtdb

#endif  // RTDB_SRC_PLATFORM_NET_HPP_
