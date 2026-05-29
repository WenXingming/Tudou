// ============================================================================
// Socket.h
// fd 独占 RAII 句柄。通用操作公开，socket 专属操作仅对 friend 类开放。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// Socket.h
// └── Socket
//     ├── Socket(sockFd)                            # [公有] 构造：接管已有 fd
//     ├── Socket(Socket&&)                          # [公有] 移动构造：转移 fd 所有权
//     ├── operator=(Socket&&)                       # [公有] 移动赋值：关闭旧 fd 后接管新 fd
//     ├── ~Socket()                                 # [公有] 析构：关闭持有的 fd
//     ├── fd() const                                # [公有] 返回裸 fd
//     ├── create_tcp_listener(addr)                 # [私有/friend Acceptor] 静态工厂：创建监听 socket
//     │   ├── socket() + SO_REUSEADDR               # [系统] 创建 non-blocking + cloexec TCP socket
//     │   ├── bind()                                # [系统] 绑定监听地址
//     │   └── listen()                              # [系统] 进入监听状态
//     ├── accept(peerAddr) const                    # [私有/friend Acceptor] 接受新连接，返回新 Socket
//     │   └── accept4(SOCK_NONBLOCK|SOCK_CLOEXEC)   # [系统] 原子创建 non-blocking 连接 socket
//     ├── set_reuse_addr(on)                        # [私有] 配置 SO_REUSEADDR（仅 create_tcp_listener 内部调用）
//     ├── set_tcp_no_delay(on)                      # [私有/friend TcpConnection] 配置 TCP_NODELAY
//     ├── set_keep_alive(on)                        # [私有/friend TcpConnection] 配置 SO_KEEPALIVE 及 TCP keepalive 参数
//     ├── shutdown_write()                          # [私有/friend TcpConnection] 半关闭写端（SHUT_WR）
//     └── local_address() const                     # [私有/friend TcpServer] 查询本地地址（getsockname）
// ============================================================================

#pragma once

#include "base/InetAddress.h"

class Acceptor;
class TcpConnection;
class TcpServer;

// Socket 是 fd 的独占 RAII 句柄。通用操作（fd/move/析构）公开，socket 专属操作仅对授权类开放。
class Socket {
public:
    explicit Socket(int sockFd) noexcept;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const { return sockFd_; }

private:
    friend class Acceptor;
    friend class TcpConnection;
    friend class TcpServer;

    // friend Acceptor 调用 create_tcp_listener() 创建监听 socket，调用 accept() 接受新连接。
    static Socket create_tcp_listener(const InetAddress& addr);
    Socket accept(sockaddr_in* peerAddr) const;

    void set_reuse_addr(bool on);

    // friend TcpConnection 调用 set_tcp_no_delay() 配置 TCP_NODELAY，调用 set_keep_alive() 配置 SO_KEEPALIVE 和 TCP keepalive 参数，调用 shutdown_write() 半关闭写端。
    void set_tcp_no_delay(bool on);
    void set_keep_alive(bool on);
    void shutdown_write();

    // friend TcpServer 调用 local_address() 查询监听 socket 的本地地址，供上层日志记录和连接发布使用。
    InetAddress local_address() const;

    int sockFd_; // 持有的 socket fd，-1 表示无效/已移动
};
