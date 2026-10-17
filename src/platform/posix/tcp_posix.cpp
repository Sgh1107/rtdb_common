/// \file POSIX（Linux）TCP 传输实现（阻塞 socket + socket 级超时模型，
/// docs/13 §3-D3）。连接建立用非阻塞 connect + poll 实现超时控制；
/// 发送禁用 SIGPIPE（MSG_NOSIGNAL）。

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

#include "platform/net.hpp"

namespace rtdb {
namespace platform {

namespace {

constexpr uintptr_t kInvalidHandle = static_cast<uintptr_t>(-1);

int AsFd(uintptr_t h) noexcept { return static_cast<int>(static_cast<intptr_t>(h)); }

uintptr_t PackFd(int fd) noexcept { return static_cast<uintptr_t>(static_cast<intptr_t>(fd)); }

int PollTimeout(uint32_t timeout_ms) noexcept {
    if (timeout_ms == kInfiniteTimeoutMs) return -1;
    return static_cast<int>(std::min<uint32_t>(timeout_ms, 0x7FFFFFFFu));
}

timeval ToTimeval(uint32_t timeout_ms) noexcept {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000u);
    tv.tv_usec = static_cast<suseconds_t>(timeout_ms % 1000u) * 1000;
    return tv;
}

}  // namespace

Err NetStartup() noexcept { return Err::kOk; }  // POSIX 无全局初始化

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
        const int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;

        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        bool connected = false;
        const int rc = connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc == 0) {
            connected = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int pr = poll(&pfd, 1, PollTimeout(timeout_ms));
            if (pr > 0 && (pfd.revents & POLLOUT) != 0) {
                int so_err = 0;
                socklen_t so_len = sizeof(so_err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) == 0 && so_err == 0) {
                    connected = true;
                }
            } else if (pr == 0) {
                last = Err::kTimeout;
            }
        }

        if (connected) {
            fcntl(fd, F_SETFL, flags);  // 恢复阻塞模式
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            freeaddrinfo(res);
            TcpSocket out;
            out.handle_ = PackFd(fd);
            out.timeout_ms_ = 0;
            return Result<TcpSocket>::Ok(std::move(out));
        }
        close(fd);
    }
    freeaddrinfo(res);
    return Result<TcpSocket>::Fail(last);
}

void TcpSocket::SetTimeout(uint32_t timeout_ms) noexcept {
    timeout_ms_ = (timeout_ms == kInfiniteTimeoutMs) ? 0 : timeout_ms;
    if (!valid()) return;
    const timeval tv = ToTimeval(timeout_ms_);
    setsockopt(AsFd(handle_), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(AsFd(handle_), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

Err TcpSocket::SendAll(const void* data, size_t len) noexcept {
    if (!valid()) return Err::kNetworkError;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        const size_t chunk = std::min<size_t>(len - sent, 1u << 30);
        const ssize_t rc = send(AsFd(handle_), p + sent, chunk, MSG_NOSIGNAL);
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
            continue;
        }
        if (rc == 0) return Err::kNetworkError;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (timeout_ms_ != 0) return Err::kTimeout;
            continue;
        }
        return Err::kNetworkError;
    }
    return Err::kOk;
}

Err TcpSocket::RecvExact(void* buf, size_t len) noexcept {
    if (!valid()) return Err::kNetworkError;
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        const size_t chunk = std::min<size_t>(len - got, 1u << 30);
        const ssize_t rc = recv(AsFd(handle_), p + got, chunk, 0);
        if (rc > 0) {
            got += static_cast<size_t>(rc);
            continue;
        }
        if (rc == 0) return Err::kNetworkError;  // 对端关闭
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (timeout_ms_ != 0) return Err::kTimeout;
            continue;
        }
        return Err::kNetworkError;
    }
    return Err::kOk;
}

void TcpSocket::Shutdown() noexcept {
    if (valid()) shutdown(AsFd(handle_), SHUT_RDWR);
}

void TcpSocket::Close() noexcept {
    if (valid()) {
        close(AsFd(handle_));
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
        const int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, it->ai_addr, it->ai_addrlen) != 0) {
            close(fd);
            continue;
        }
        if (listen(fd, backlog) != 0) {
            close(fd);
            continue;
        }
        sockaddr_storage ss{};
        socklen_t ss_len = sizeof(ss);
        uint16_t bound_port = port;
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &ss_len) == 0) {
            if (ss.ss_family == AF_INET) {
                bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
            }
        }
        freeaddrinfo(res);
        TcpListener out;
        out.handle_ = PackFd(fd);
        out.port_ = bound_port;
        return Result<TcpListener>::Ok(std::move(out));
    }
    freeaddrinfo(res);
    return Result<TcpListener>::Fail(Err::kNetworkError);
}

Result<TcpSocket> TcpListener::Accept(uint32_t timeout_ms) noexcept {
    if (!valid()) return Result<TcpSocket>::Fail(Err::kNetworkError);
    pollfd pfd{};
    pfd.fd = AsFd(handle_);
    pfd.events = POLLIN;
    const int pr = poll(&pfd, 1, PollTimeout(timeout_ms));
    if (pr == 0) return Result<TcpSocket>::Fail(Err::kTimeout);
    if (pr < 0) return Result<TcpSocket>::Fail(Err::kNetworkError);

    const int fd = accept(AsFd(handle_), nullptr, nullptr);
    if (fd < 0) {
        return Result<TcpSocket>::Fail((errno == EAGAIN || errno == EWOULDBLOCK) ? Err::kTimeout
                                                                                 : Err::kNetworkError);
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    TcpSocket out;
    out.handle_ = PackFd(fd);
    out.timeout_ms_ = 0;
    return Result<TcpSocket>::Ok(std::move(out));
}

void TcpListener::Close() noexcept {
    if (valid()) {
        close(AsFd(handle_));
        handle_ = kInvalidHandle;
        port_ = 0;
    }
}

}  // namespace platform
}  // namespace rtdb
