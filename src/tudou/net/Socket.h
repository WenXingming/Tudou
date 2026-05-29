// ============================================================================
// Socket.h
// 网络 socket RAII 封装，负责底层 socket 系统调用的统一收口。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// Socket.h
// └── Socket
//     ├── Socket(sockFd)                            # [公有] 构造：接管已有 fd（通常来自 accept4）
//     ├── Socket(Socket&&)                          # [公有] 移动构造：转移 fd 所有权
//     ├── operator=(Socket&&)                       # [公有] 移动赋值：关闭旧 fd 后接管新 fd
//     ├── ~Socket()                                 # [公有] 析构：关闭持有的 fd
//     ├── fd() const                                # [公有] 返回裸 fd（供 Channel/epoll 注册使用）
//     ├── create_tcp_listener(addr)                 # [公有] 静态工厂：创建监听 socket
//     │   ├── socket() + SO_REUSEADDR               # [系统] 创建 non-blocking + cloexec TCP socket
//     │   ├── bind()                                # [系统] 绑定监听地址
//     │   └── listen()                              # [系统] 进入监听状态
//     ├── accept(peerAddr) const                    # [公有] 接受新连接，返回新 Socket
//     │   └── accept4(SOCK_NONBLOCK|SOCK_CLOEXEC)   # [系统] 原子创建 non-blocking 连接 socket
//     ├── set_tcp_no_delay(on)                      # [公有] 配置 TCP_NODELAY（禁用 Nagle）
//     ├── set_keep_alive(on)                        # [公有] 配置 SO_KEEPALIVE 及 TCP keepalive 参数
//     ├── set_reuse_addr(on)                        # [公有] 配置 SO_REUSEADDR
//     ├── shutdown_write()                          # [公有] 半关闭写端（SHUT_WR）
//     └── local_address() const                     # [公有] 查询本地地址（getsockname）
// ============================================================================

#pragma once

#include "base/InetAddress.h"

// Socket 是网络 socket fd 的独占 RAII 句柄。只封装底层系统调用，不持有 EventLoop/Channel/回调。
class Socket {
public:
    explicit Socket(int sockFd) noexcept;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const { return sockFd_; }

    // 创建 non-blocking + cloexec 的 TCP 监听 socket，绑定并进入 listen 状态。
    // 自动设置 SO_REUSEADDR 避免重启时 TIME_WAIT 导致 bind 失败。
    static Socket create_tcp_listener(const InetAddress& addr);

    // 接受新连接，返回 non-blocking + cloexec 的 Socket。peerAddr 非空时写入对端原始地址。
    Socket accept(sockaddr_in* peerAddr) const;

    void set_reuse_addr(bool on);
    void set_tcp_no_delay(bool on);
    void set_keep_alive(bool on);   // 同时设置 SO_KEEPALIVE 和 TCP keepalive 参数
    void shutdown_write();

    // 查询本地地址（getsockname），用于新连接建立时获取本端地址快照。
    InetAddress local_address() const;

private:
    int sockFd_; // 持有的 socket fd，-1 表示无效/已移动
};
