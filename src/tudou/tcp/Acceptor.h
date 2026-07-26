// ============================================================================
// Acceptor 负责监听 socket 的事件编排和新连接发布。
// 底层 socket 创建、接收和 fd 所有权由 Socket 负责。
//
// ============================================================================

#pragma once

#include <functional>
#include <memory>

#include "tudou/tcp/InetAddress.h"
#include "tudou/tcp/Socket.h"

class EventLoop;
class Channel;

class Acceptor {
public:
    using NewConnectCallback = std::function<void(Socket connSocket, const InetAddress& peerAddr)>;

    explicit Acceptor(EventLoop* loop, const InetAddress& listenAddr);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    // 连接发布和监听状态查询。
    void set_connect_callback(NewConnectCallback cb);
    int get_listen_fd() const;

private:
    void on_read(Channel& channel);          // 监听 fd 可读后的统一接入入口。
    void handle_connect_callback(Socket connSocket, const InetAddress& peerAddr);
    void accept_idle_connection();            // fd 耗尽恢复：关闭预留 fd、接收并关闭连接、重建预留 fd。

private:
    EventLoop* loop_;                       // Acceptor 运行所在的事件循环（线程），负责调度事件回调

    Socket listenSocket_;                   // 监听 socket 的 RAII 句柄，析构时自动关闭 fd
    std::unique_ptr<Channel> channel_;      // 监听 socket 对应的事件通道（声明在后，确保析构时先反注册再关 fd）
    Socket idleFd_;                         // 预留的 /dev/null fd，fd 耗尽时关闭它以恢复 accept。

    NewConnectCallback newConnectCallback_;
};
