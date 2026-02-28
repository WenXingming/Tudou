/**
 * @file Acceptor.h
 * @brief 监听新连接的接入器，封装 listenFd 及其 Channel，accept 后直接通过回调将 connFd/peerAddr 传给上层。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 * 职责：创建、绑定、监听套接字；在可读事件触发时 accept 新连接，通过
 * NewConnectCallback(int connFd, const InetAddress& peerAddr) 直接将连接信息
 * 传递给上层（TcpServer），不暂存中间状态。
 *
 * 线程模型：与所属 EventLoop 线程绑定。仅保存上层回调，不持有上层对象。
 * 生命周期：持有 Channel（unique_ptr），析构时自动解除注册；connFd 仅发布，
 * 后续由上层 TcpConnection 管理。
 */

#pragma once
#include <functional>
#include <memory>
#include "base/InetAddress.h"

class EventLoop;
class Channel;
class InetAddress;
class Acceptor {
    // accept 后直接将 connFd 和 peerAddr 作为参数传递给上层，无需暂存中间状态
    using NewConnectCallback = std::function<void(int connFd, const InetAddress& peerAddr)>;

public:
    Acceptor(EventLoop* loop, const InetAddress& listenAddr);
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    ~Acceptor();

    void set_connect_callback(NewConnectCallback cb);
    int get_listen_fd() const;

private:
    int create_fd();
    void bind_address(int listenFd);
    void start_listen(int listenFd);

    void on_error(Channel& channel);
    void on_close(Channel& channel);
    void on_write(Channel& channel);
    void on_read(Channel& channel); // 有新连接到来，循环 accept

    void handle_connect_callback(int connFd, const InetAddress& peerAddr); // 触发上层回调

private:
    EventLoop* loop_;
    InetAddress listenAddr_;
    int listenFd_;                          // 注：只使用，不负责生命周期管理
    std::unique_ptr<Channel> channel_;
    NewConnectCallback newConnectCallback_;
};
