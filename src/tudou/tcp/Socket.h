// ============================================================================
// Socket.h
// Socket 专属操作的封装，生命周期托管给 ScopedFd。
// ============================================================================

#pragma once

#include "base/ScopedFd.h"
#include "tudou/tcp/InetAddress.h"

class Acceptor;
class TcpConnection;
class TcpServer;

class Socket {
public:
    explicit Socket(int sockFd) noexcept;
    
    // 依赖编译器自动生成的移动和析构逻辑（Rule of Zero）
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;
    ~Socket() = default;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const { return fd_.fd(); }

    static Socket create_tcp_listener(const InetAddress& addr);
    Socket accept(sockaddr_in* peerAddr) const;

    void set_reuse_addr(bool on);
    void set_tcp_no_delay(bool on);
    void set_keep_alive(bool on);
    void shutdown_write();

    InetAddress local_address() const;

private:
    ScopedFd fd_;
};
