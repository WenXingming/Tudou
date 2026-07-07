// ============================================================================
// Socket.cpp
// 网络 socket RAII 封装实现，集中管理 socket 系统调用的错误处理。
// ============================================================================

#include "tudou/tcp/Socket.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include "spdlog/spdlog.h"

Socket::Socket(int sockFd) noexcept
    : fd_(sockFd) {
}

Socket Socket::create_tcp_listener(const InetAddress& addr) {
    const int listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listenFd < 0) {
        std::string errMsg = "Socket::create_tcp_listener(): socket() failed, errno=" + std::to_string(errno) + " (" + strerror(errno) + ")";
        spdlog::error(errMsg);
        throw std::runtime_error(errMsg);
    }

    // 包装为 Socket 后，后续失败路径由析构兜底关闭 fd。
    Socket sock(listenFd);

    // 设置 SO_REUSEADDR，避免服务器重启时 TIME_WAIT 导致 bind 失败
    sock.set_reuse_addr(true);

    sockaddr_in rawAddr = addr.get_sockaddr();
    if (::bind(sock.fd(), reinterpret_cast<sockaddr*>(&rawAddr), sizeof(rawAddr)) < 0) {
        std::string errMsg = "Socket::create_tcp_listener(): bind() failed, errno=" + std::to_string(errno) + " (" + strerror(errno) + ")";
        spdlog::error(errMsg);
        throw std::runtime_error(errMsg);
    }

    if (::listen(sock.fd(), SOMAXCONN) < 0) {
        std::string errMsg = "Socket::create_tcp_listener(): listen() failed, errno=" + std::to_string(errno) + " (" + strerror(errno) + ")";
        spdlog::error(errMsg);
        throw std::runtime_error(errMsg);
    }

    return sock;
}

Socket Socket::accept(sockaddr_in* peerAddr) const {
    socklen_t addrLen = sizeof(sockaddr_in);
    // 使用 accept4 一次性原子创建 non-blocking + cloexec 连接 socket，减少系统调用并防 fd 泄露。
    const int connFd = ::accept4(fd(),
                                 reinterpret_cast<sockaddr*>(peerAddr),
                                 &addrLen,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (connFd < 0) {
        // 在高并发连接瞬时断开（如收到 RST）时，accept4 可能会失败返回 EAGAIN/EWOULDBLOCK/EINTR/ECONNABORTED
        // 这些不是致命的系统异常，只需记录日志并由调用方上层安全重试。
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
            spdlog::error("Socket::accept(): accept4() failed, errno={} ({})", errno, strerror(errno));
        }
    }

    return Socket(connFd);
}

void Socket::set_reuse_addr(bool on) {
    const int kEnable = on ? 1 : 0;
    if (::setsockopt(fd(), SOL_SOCKET, SO_REUSEADDR, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("Socket: failed to set SO_REUSEADDR on fd {}, errno: {}", fd(), errno);
    }
}

void Socket::set_tcp_no_delay(bool on) {
    const int kEnable = on ? 1 : 0;
    if (::setsockopt(fd(), IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("Socket: failed to set TCP_NODELAY on fd {}, errno: {}", fd(), errno);
    }
}

void Socket::set_keep_alive(bool on) {
    const int kEnable = on ? 1 : 0;
    if (::setsockopt(fd(), SOL_SOCKET, SO_KEEPALIVE, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("Socket: failed to set SO_KEEPALIVE on fd {}, errno: {}", fd(), errno);
        return;
    }

    if (!on) {
        return;
    }

    // 在保持连接活动时，定制较短的 TCP keepalive 探测间隔，及时剔除僵尸连接。
    // 这比 Linux 默认的 2 小时探测要高效得多，更契合高并发网络服务。
    const int kKeepIdleSec = 30;     // 闲置 30s 开始探测
    const int kKeepIntvlSec = 6;     // 探测间隔 6s
    const int kKeepCnt = 3;          // 连续 3 次无响应认为连接断开

    if (::setsockopt(fd(), IPPROTO_TCP, TCP_KEEPIDLE, &kKeepIdleSec, sizeof(kKeepIdleSec)) < 0) {
        spdlog::warn("Socket: failed to set TCP_KEEPIDLE on fd {}, errno: {}", fd(), errno);
    }
    if (::setsockopt(fd(), IPPROTO_TCP, TCP_KEEPINTVL, &kKeepIntvlSec, sizeof(kKeepIntvlSec)) < 0) {
        spdlog::warn("Socket: failed to set TCP_KEEPINTVL on fd {}, errno: {}", fd(), errno);
    }
    if (::setsockopt(fd(), IPPROTO_TCP, TCP_KEEPCNT, &kKeepCnt, sizeof(kKeepCnt)) < 0) {
        spdlog::warn("Socket: failed to set TCP_KEEPCNT on fd {}, errno: {}", fd(), errno);
    }
}

void Socket::shutdown_write() {
    if (::shutdown(fd(), SHUT_WR) < 0) {
        spdlog::warn("Socket: failed to shutdown_write on fd {}, errno: {}", fd(), errno);
    }
}

InetAddress Socket::local_address() const {
    sockaddr_in localSockAddr;
    memset(&localSockAddr, 0, sizeof(localSockAddr));
    socklen_t addrLen = sizeof(localSockAddr);

    if (::getsockname(fd(), reinterpret_cast<sockaddr*>(&localSockAddr), &addrLen) < 0) {
        spdlog::error("Socket::local_address(): getsockname() failed, errno={} ({})", errno, strerror(errno));
    }

    return InetAddress(localSockAddr);
}
