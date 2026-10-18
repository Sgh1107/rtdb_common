/// \file Win32 TCP 传输实现（Winsock2；阻塞 socket + socket 级超时模型，
/// docs/13 §3-D3）。连接建立用非阻塞 connect + select 实现超时控制。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

#include "platform/net.hpp"

namespace rtdb {
namespace platform {

namespace {

constexpr uintptr_t kInvalidHandle = static_cast<uintptr_t>(-1);

Err EnsureWinsock() noexcept {
    static std::once_flag once;
    static int ws_err = 0;
    std::call_once(once, []() noexcept {
        WSADATA wsa;
        ws_err = WSAStartup(MAKEWORD(2, 2), &wsa);
    });
    return ws_err == 0 ? Err::kOk : Err::kNetworkError;
}

SOCKET AsSocket(uintptr_t h) noexcept { return static_cast<SOCKET>(h); }

timeval ToTimeval(uint32_t timeout_ms) noexcept {
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000u);
    tv.tv_usec = static_cast<long>(timeout_ms % 1000u) * 1000L;
    return tv;
}

/// select 超时参数：kInfiniteTimeoutMs 返回 nullptr（阻塞等待）。
timeval* SelectTimeout(uint32_t timeout_ms, timeval* storage) noexcept {
    if (timeout_ms == kInfiniteTimeoutMs) return nullptr;
    *storage = ToTimeval(timeout_ms);
    return storage;
}

}  // namespace

Err NetStartup() noexcept { return EnsureWinsock(); }

// ---------------------------------------------------------------- TcpSocket

TcpSocket::~TcpSocket() { Close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : handle_(other.handle_), timeout_ms_(other.timeout_ms_) {
    other.handle_ = kInvalidHandle;
    other.timeout_ms_ = 0;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        timeout_ms_ = other.timeout_ms_;
        other.handle_ = kInvalidHandle;
        other.timeout_ms_ = 0;
    }
    return *this;
}

bool TcpSocket::valid() const noexcept { return handle_ != kInvalidHandle; }

Result<TcpSocket> TcpSocket::Connect(const std::string& host, uint16_t port,
                                     uint32_t timeout_ms) noexcept {
    if (EnsureWinsock() != Err::kOk) return Result<TcpSocket>::Fail(Err::kNetworkError);

    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
        return Result<TcpSocket>::Fail(Err::kNetworkError);
    }

    Err last = Err::kNetworkError;
    for (addrinfo* it = res; it != nullptr; it = it->ai_next) {
        const SOCKET s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);

        bool connected = false;
        const int rc = connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen));
        if (rc == 0) {
            connected = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            // Winsock 语义：连接**成功**在 writefds 上报，连接**失败**在
            // exceptfds 上报——两个集都要监视，再以 SO_ERROR 判定结果。
            fd_set wf;
            fd_set ef;
            FD_ZERO(&wf);
            FD_ZERO(&ef);
            FD_SET(s, &wf);
            FD_SET(s, &ef);
            timeval tv_storage{};
            timeval* ptv = SelectTimeout(timeout_ms, &tv_storage);
            const int sel = select(0, nullptr, &wf, &ef, ptv);
            if (sel > 0 && (FD_ISSET(s, &wf) || FD_ISSET(s, &ef))) {
                int so_err = 0;
                int so_len = static_cast<int>(sizeof(so_err));
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err),
                               &so_len) == 0 &&
                    so_err == 0) {
                    connected = true;
                }
            } else if (sel == 0) {
                last = Err::kTimeout;
            }
        }

        if (connected) {
            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                       sizeof(one));
            freeaddrinfo(res);
            TcpSocket out;
            out.handle_ = static_cast<uintptr_t>(s);
            out.timeout_ms_ = 0;
            return Result<TcpSocket>::Ok(std::move(out));
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    return Result<TcpSocket>::Fail(last);
}

void TcpSocket::SetTimeout(uint32_t timeout_ms) noexcept {
    timeout_ms_ = (timeout_ms == kInfiniteTimeoutMs) ? 0 : timeout_ms;
    if (!valid()) return;
    const DWORD ms = static_cast<DWORD>(timeout_ms_);  // 0 = 无限（Windows 语义）
    setsockopt(AsSocket(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
               sizeof(ms));
    setsockopt(AsSocket(handle_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms),
               sizeof(ms));
}

Err TcpSocket::SendAll(const void* data, size_t len) noexcept {
    if (!valid()) return Err::kNetworkError;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        const int chunk = static_cast<int>(std::min<size_t>(len - sent, 1u << 30));
        const int rc = send(AsSocket(handle_), reinterpret_cast<const char*>(p + sent), chunk, 0);
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
            continue;
        }
        if (rc == 0) return Err::kNetworkError;
        const int e = WSAGetLastError();
        if (e == WSAEINTR) continue;
        if (e == WSAEWOULDBLOCK) {
            // 部分发送后超时：流已含半帧，调用方不能按"未发送"重试。
            if (timeout_ms_ != 0) return sent > 0 ? Err::kNetworkError : Err::kTimeout;
            continue;
        }
        if (e == WSAETIMEDOUT) return sent > 0 ? Err::kNetworkError : Err::kTimeout;
        return Err::kNetworkError;
    }
    return Err::kOk;
}

Err TcpSocket::RecvExact(void* buf, size_t len) noexcept {
    if (!valid()) return Err::kNetworkError;
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        const int chunk = static_cast<int>(std::min<size_t>(len - got, 1u << 30));
        const int rc = recv(AsSocket(handle_), reinterpret_cast<char*>(p + got), chunk, 0);
        if (rc > 0) {
            got += static_cast<size_t>(rc);
            continue;
        }
        if (rc == 0) return Err::kNetworkError;  // 对端关闭
        const int e = WSAGetLastError();
        if (e == WSAEINTR) continue;
        if (e == WSAEWOULDBLOCK) {
            // 部分接收后超时：已消费字节无法回退，半帧一律按连接失效处理，
            // 避免上层把超时当"空闲"而帧边界错位。
            if (timeout_ms_ != 0) return got > 0 ? Err::kNetworkError : Err::kTimeout;
            continue;
        }
        if (e == WSAETIMEDOUT) return got > 0 ? Err::kNetworkError : Err::kTimeout;
        return Err::kNetworkError;
    }
    return Err::kOk;
}

void TcpSocket::Shutdown() noexcept {
    if (valid()) shutdown(AsSocket(handle_), SD_BOTH);
}

void TcpSocket::Close() noexcept {
    if (valid()) {
        closesocket(AsSocket(handle_));
        handle_ = kInvalidHandle;
        timeout_ms_ = 0;
    }
}

// -------------------------------------------------------------- TcpListener

TcpListener::~TcpListener() { Close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_) {
    other.handle_ = kInvalidHandle;
    other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = kInvalidHandle;
        other.port_ = 0;
    }
    return *this;
}

bool TcpListener::valid() const noexcept { return handle_ != kInvalidHandle; }

Result<TcpListener> TcpListener::Listen(const std::string& bind_addr, uint16_t port,
                                        int backlog) noexcept {
    if (EnsureWinsock() != Err::kOk) return Result<TcpListener>::Fail(Err::kNetworkError);

    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    // 安全默认：空地址 = 仅回环（docs/13 §7 风险对策）。
    const std::string addr = bind_addr.empty() ? std::string("127.0.0.1") : bind_addr;
    addrinfo* res = nullptr;
    if (getaddrinfo(addr.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
        return Result<TcpListener>::Fail(Err::kNetworkError);
    }

    for (addrinfo* it = res; it != nullptr; it = it->ai_next) {
        const SOCKET s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        // Windows 不设 SO_REUSEADDR（默认排他绑定，防端口劫持）。
        if (bind(s, it->ai_addr, static_cast<int>(it->ai_addrlen)) != 0) {
            closesocket(s);
            continue;
        }
        if (listen(s, backlog) != 0) {
            closesocket(s);
            continue;
        }
        sockaddr_storage ss{};
        int ss_len = static_cast<int>(sizeof(ss));
        uint16_t bound_port = port;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&ss), &ss_len) == 0) {
            if (ss.ss_family == AF_INET) {
                bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
            }
        }
        freeaddrinfo(res);
        TcpListener out;
        out.handle_ = static_cast<uintptr_t>(s);
        out.port_ = bound_port;
        return Result<TcpListener>::Ok(std::move(out));
    }
    freeaddrinfo(res);
    return Result<TcpListener>::Fail(Err::kNetworkError);
}

Result<TcpSocket> TcpListener::Accept(uint32_t timeout_ms) noexcept {
    if (!valid()) return Result<TcpSocket>::Fail(Err::kNetworkError);
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(AsSocket(handle_), &rf);
    timeval tv_storage{};
    timeval* ptv = SelectTimeout(timeout_ms, &tv_storage);
    const int sel = select(0, &rf, nullptr, nullptr, ptv);
    if (sel == 0) return Result<TcpSocket>::Fail(Err::kTimeout);
    if (sel == SOCKET_ERROR) return Result<TcpSocket>::Fail(Err::kNetworkError);

    const SOCKET s = accept(AsSocket(handle_), nullptr, nullptr);
    if (s == INVALID_SOCKET) {
        return Result<TcpSocket>::Fail(WSAGetLastError() == WSAETIMEDOUT ? Err::kTimeout
                                                                          : Err::kNetworkError);
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    TcpSocket out;
    out.handle_ = static_cast<uintptr_t>(s);
    out.timeout_ms_ = 0;
    return Result<TcpSocket>::Ok(std::move(out));
}

void TcpListener::Close() noexcept {
    if (valid()) {
        closesocket(AsSocket(handle_));
        handle_ = kInvalidHandle;
        port_ = 0;
    }
}

}  // namespace platform
}  // namespace rtdb
