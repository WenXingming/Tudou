// ============================================================================
// Socket.cpp
// 网络 socket RAII 封装实现，集中管理 socket 系统调用的错误处理。
// ============================================================================

#include "Socket.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "spdlog/spdlog.h"

Socket::Socket(int sockFd) noexcept
    : sockFd_(sockFd) {
}

Socket::Socket(Socket&& other) noexcept
    : sockFd_(other.sockFd_) {
    other.sockFd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        // 先关闭当前持有的 fd，再接管新 fd
        if (sockFd_ >= 0) {
            ::close(sockFd_);
        }
        sockFd_ = other.sockFd_;
        other.sockFd_ = -1;
    }
    return *this;
}

Socket::~Socket() {
    if (sockFd_ >= 0) {
        ::close(sockFd_);
    }
}

Socket Socket::create_tcp_listener(const InetAddress& addr) {
    const int listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listenFd < 0) {
        spdlog::error("Socket::create_tcp_listener(): socket() failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }

    // 包装为 Socket 后，后续失败路径由析构兜底关闭 fd（assert 直接终止，析构不执行）。
    Socket sock(listenFd);

    // 设置 SO_REUSEADDR，避免服务器重启时 TIME_WAIT 导致 bind 失败
    sock.set_reuse_addr(true);

    sockaddr_in rawAddr = addr.get_sockaddr();
    if (::bind(sock.fd(), reinterpret_cast<sockaddr*>(&rawAddr), sizeof(rawAddr)) < 0) {
        spdlog::error("Socket::create_tcp_listener(): bind() failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }

    if (::listen(sock.fd(), SOMAXCONN) < 0) {
        spdlog::error("Socket::create_tcp_listener(): listen() failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }

    return sock;
}

Socket Socket::accept(sockaddr_in* peerAddr) const {
    sockaddr_in clientAddr{};
    socklen_t length = sizeof(clientAddr);
    const int connFd = ::accept4(sockFd_,
        reinterpret_cast<sockaddr*>(&clientAddr),
        &length,
        SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connFd < 0) {
        spdlog::error("Socket::accept(): accept4() failed, errno={} ({})", errno, strerror(errno));
        return Socket(-1);
    }

    if (peerAddr != nullptr) *peerAddr = clientAddr;
    return Socket(connFd);
}

void Socket::set_reuse_addr(bool on) {
    const int kEnable = on ? 1 : 0;
    if (::setsockopt(sockFd_, SOL_SOCKET, SO_REUSEADDR, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("Socket: failed to set SO_REUSEADDR on fd {}, errno: {}", sockFd_, errno);
    }
}

void Socket::set_tcp_no_delay(bool on) {
    const int kEnable = on ? 1 : 0;
    if (::setsockopt(sockFd_, IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("Socket: failed to set TCP_NODELAY on fd {}, errno: {}", sockFd_, errno);
    }
}

void Socket::set_keep_alive(bool on) {
    const int kEnable = on ? 1 : 0;
    if (::setsockopt(sockFd_, SOL_SOCKET, SO_KEEPALIVE, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("Socket: failed to set SO_KEEPALIVE on fd {}, errno: {}", sockFd_, errno);
        return;
    }

    if (!on) {
        return;
    }

    // TCP keepalive 参数：空闲 60s 开始探测，探测间隔 10s，最多 3 次
    constexpr int kKeepIdleSec = 60;
    constexpr int kKeepIntvlSec = 10;
    constexpr int kKeepCnt = 3;

    if (::setsockopt(sockFd_, IPPROTO_TCP, TCP_KEEPIDLE, &kKeepIdleSec, sizeof(kKeepIdleSec)) < 0) {
        spdlog::warn("Socket: failed to set TCP_KEEPIDLE on fd {}, errno: {}", sockFd_, errno);
    }
    if (::setsockopt(sockFd_, IPPROTO_TCP, TCP_KEEPINTVL, &kKeepIntvlSec, sizeof(kKeepIntvlSec)) < 0) {
        spdlog::warn("Socket: failed to set TCP_KEEPINTVL on fd {}, errno: {}", sockFd_, errno);
    }
    if (::setsockopt(sockFd_, IPPROTO_TCP, TCP_KEEPCNT, &kKeepCnt, sizeof(kKeepCnt)) < 0) {
        spdlog::warn("Socket: failed to set TCP_KEEPCNT on fd {}, errno: {}", sockFd_, errno);
    }
}

void Socket::shutdown_write() {
    if (::shutdown(sockFd_, SHUT_WR) < 0) {
        spdlog::warn("Socket: failed to shutdown_write on fd {}, errno: {}", sockFd_, errno);
    }
}

InetAddress Socket::local_address() const {
    sockaddr_in localSockAddr{};
    socklen_t addrLen = sizeof(localSockAddr);
    if (::getsockname(sockFd_, reinterpret_cast<sockaddr*>(&localSockAddr), &addrLen) < 0) {
        spdlog::error("Socket::local_address(): getsockname() failed, errno={} ({})", errno, strerror(errno));
    }
    return InetAddress(localSockAddr);
}
