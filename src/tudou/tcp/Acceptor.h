// ============================================================================
// Acceptor.h
// 监听接入器，负责监听侧编排（epoll 事件处理、瞬时错误过滤、连接发布），
// 底层 socket 操作委托给 Socket 类。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// Acceptor.h
// └── Acceptor
//     ├── Acceptor(loop, listenAddr)              # [公有] 构造：创建 Socket 监听器并绑定 Channel 回调
//     │   ├── Socket::create_tcp_listener(addr)   # [Socket] 创建 non-blocking 监听 socket 并 bind+listen
//     │   └── bind_channel_callbacks()            # [私有] 把 read/error/close/write 事件绑定到成员函数
//     │       ├── on_read(channel)                # [私有] 监听 socket 可读时的 accept 入口
//     │       │   ├── Socket::accept(&peerAddr)   # [Socket] accept4 返回新 Socket
//     │       │   ├── is_transient_accept_error() # [私有] 忽略 EAGAIN/EINTR 等瞬时错误
//     │       │   └── publish_connection(sock, addr) # [私有] 包装对端地址并继续向上发布
//     │       │       └── handle_connect_callback(...) # [私有] 触发上层 newConnectCallback_
//     │       ├── on_error(channel)               # [私有] 监听 fd 错误分支，仅做诊断
//     │       ├── on_close(channel)               # [私有] 监听 fd 异常关闭分支
//     │       └── on_write(channel)               # [私有] 监听 fd 异常写事件分支
//     ├── ~Acceptor()                             # [公有] 析构：listenSocket_ 和 channel_ 按序销毁
//     ├── set_connect_callback(cb)                # [公有] 注册 accept 成功后的上行发布回调
//     └── get_listen_fd() const                   # [公有] 返回监听 fd
// ============================================================================

#pragma once

#include <functional>
#include <memory>

#include "base/InetAddress.h"
#include "Socket.h"

class EventLoop;
class Channel;

// Acceptor 负责"监听并发布新连接"的编排，Socket 负责底层 socket 操作。
class Acceptor {
public:
    using NewConnectCallback = std::function<void(Socket connSocket, const InetAddress& peerAddr)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr);
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    ~Acceptor();

    void set_connect_callback(NewConnectCallback cb);
    int get_listen_fd() const;

private:
    void bind_channel_callbacks();
    void on_error(Channel& channel);
    void on_close(Channel& channel);
    void on_write(Channel& channel);
    void on_read(Channel& channel);          // 监听 fd 可读后的统一接入入口。
    bool is_transient_accept_error(int errorCode) const;
    void publish_connection(Socket connSocket, const InetAddress& peerAddr);
    void handle_connect_callback(Socket connSocket, const InetAddress& peerAddr);

private:
    EventLoop* loop_;
    Socket listenSocket_;                    // 监听 socket 的 RAII 句柄，析构时自动关闭 fd
    std::unique_ptr<Channel> channel_;       // 监听 socket 对应的事件通道（声明在后，确保析构时先反注册再关 fd）
    NewConnectCallback newConnectCallback_;
};
