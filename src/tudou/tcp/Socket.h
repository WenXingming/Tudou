// ============================================================================
// Socket.h
// Socket 专属操作的封装，生命周期托管给 ScopedFd。
// ============================================================================

#pragma once

#include "base/ScopedFd.h"
#include "tudou/tcp/InetAddress.h"

class Socket {
public:
    explicit Socket(int sockFd) noexcept;
    ~Socket() = default;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;

    int fd() const { return fd_.fd(); }

    // 监听 socket 创建和连接接收。
    static Socket create_tcp_listener(const InetAddress& addr);
    Socket accept(sockaddr_in& peerAddr) const;

    // socket 选项和半关闭。
    void set_reuse_addr(bool on);
    void set_tcp_no_delay(bool on);
    void set_keep_alive(bool on);
    void shutdown_write();

    // 地址查询。
    InetAddress local_address() const;

private:
    ScopedFd fd_;
};
