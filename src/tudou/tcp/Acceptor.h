// ============================================================================
// Acceptor.h
// 监听接入器，负责创建监听 socket、接收新连接，并把结果直接发布给上层。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// Acceptor.h
// └── Acceptor
//     ├── Acceptor(loop, listenAddr)              # [公有] 启动监听链路并绑定 listen Channel 回调
//     │   ├── create_fd()                         # [私有] 创建 non-blocking + cloexec 监听 socket
//     │   ├── bind_address(listenFd)              # [私有] 绑定监听地址
//     │   ├── start_listen(listenFd)              # [私有] 进入 listen 状态
//     │   └── bind_channel_callbacks()            # [私有] 把 read/error/close/write 事件接到成员函数
//     │       ├── on_read(channel)                # [私有] 监听 socket 可读时的唯一接入入口
//     │       │   ├── accept_connection(&clientAddr)      # [私有] accept4 取出一个新连接
//     │       │   ├── is_transient_accept_error(errno)    # [私有] 忽略 EAGAIN/EINTR 等瞬时错误
//     │       │   └── publish_connection(connFd, addr)    # [私有] 把新连接包装后继续向上发布
//     │       │       └── handle_connect_callback(...)    # [私有] 触发上层 newConnectCallback_
//     │       ├── on_error(channel)               # [私有] 监听 fd 错误分支，只做诊断
//     │       ├── on_close(channel)               # [私有] 监听 fd 异常关闭分支
//     │       └── on_write(channel)               # [私有] 监听 fd 异常写事件分支
//     ├── Acceptor(copy)                          # [公有] 删除拷贝构造，保持监听 fd 唯一归属
//     ├── operator=(copy)                         # [公有] 删除拷贝赋值，禁止复制监听通道状态
//     ├── ~Acceptor()                             # [公有] 析构：资源由 Channel/fd 生命周期兜底清理
//     ├── set_connect_callback(cb)                # [公有] 注册 accept 成功后的上行发布回调
//     └── get_listen_fd() const                   # [公有] 返回监听 fd
// ============================================================================

#pragma once

#include <functional>
#include <memory>

#include "base/InetAddress.h"

class EventLoop;
class Channel;

// Acceptor 只负责“监听并发布新连接”，不参与连接生命周期管理。
class Acceptor {
public:
    using NewConnectCallback = std::function<void(int connFd, const InetAddress& peerAddr)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr);
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    ~Acceptor();

    void set_connect_callback(NewConnectCallback cb); // 注册 accept 成功后的上行回调。
    int get_listen_fd() const;

private:
    int create_fd();
    void bind_address(int listenFd);
    void start_listen(int listenFd);
    void bind_channel_callbacks();
    void on_error(Channel& channel);
    void on_close(Channel& channel);
    void on_write(Channel& channel);
    void on_read(Channel& channel); // 监听 fd 可读后的统一接入入口。
    int accept_connection(sockaddr_in* clientAddr) const; // accept4 取出一个新连接。
    bool is_transient_accept_error(int errorCode) const;
    void publish_connection(int connFd, const sockaddr_in& clientAddr); // 包装对端地址并向上发布。
    void handle_connect_callback(int connFd, const InetAddress& peerAddr);

private:
    EventLoop* loop_; // 所属 EventLoop，定义 Acceptor 的线程边界。
    InetAddress listenAddr_; // 监听地址契约。
    int listenFd_; // 监听 socket fd，由监听 Channel 负责最终关闭。
    std::unique_ptr<Channel> channel_; // 监听 socket 对应的事件通道。
    NewConnectCallback newConnectCallback_; // accept 成功后向上发布连接的回调。
};
