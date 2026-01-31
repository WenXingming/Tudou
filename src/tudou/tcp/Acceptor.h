/**
 * @file Acceptor.h
 * @brief 监听新连接的接入器（封装 listenFd 及持有其 Channel），在有连接到来时接受并上报给上层。
 * @author wenxingming
 * @date 2025-12-16
 * @project: https://github.com/WenXingming/Tudou
 * @details
 *
 * 职责：
 * - 封装监听套接字的创建、绑定与监听（create_fd/bind_address/listen_start）。
 * - 将 listenFd 注册到所属 EventLoop 的 Channel 上，监听可读事件（新连接到来）。
 * - 在可读事件触发时执行 read_callback，循环 accept 并生成 connFd，然后通过回调发布给上层（TcpServer）。
 * - 对外提供 set_connect_callback()，用于上层设置“新连接回调”，实现去耦。
 *
 * 线程模型与约定：
 * - 与所属 EventLoop 线程绑定，非线程安全方法需在该线程调用。
 * - 仅保存上层回调，不持有上层对象，避免环形依赖。
 *
 * 生命周期与所有权：
 * - 持有 Channel 的唯一所有权（std::unique_ptr），析构时自动解除注册与释放。
 * - 负责 listenFd 的管理；而对已接受的 connFd 仅发布，不负责其后续生命周期（由上层 TcpConnection 管理）。
 *
 * 错误处理：
 * - accept 失败时记录日志并进行必要的失败分支处理（如 EAGAIN、资源耗尽等）。
 */

#pragma once
#include <functional>
#include <memory>

#include "base/InetAddress.h"

class EventLoop;
class Channel;
class InetAddress;
class Acceptor {
    // 参数设计：上层使用下层，所以参数是下层类型，因为一般通过 composition 来使用下层类，参数一般是指针或引用
    // 回调参数为 Acceptor 引用，上层通过 Acceptor 的接口获取新连接信息（connFd 和 peerAddr）
    // 这样设计的优点：
    // 1. 高内聚：Acceptor 封装了接受连接的完整信息
    // 2. 对称性：与 TcpConnection 回调风格统一，参数都是对象引用
    // 3. 线程安全：单线程 Acceptor，回调在同一线程执行，无并发问题
    using NewConnectCallback = std::function<void(Acceptor&)>;

private:
    EventLoop* loop;
    InetAddress listenAddr;
    int listenFd; // accept() 方法被频繁调用，避免重复获取成员变量，所以 Acceptor 保存 listenFd 成员变量（注：只使用，不负责生命周期管理）
    std::unique_ptr<Channel> channel;
    NewConnectCallback newConnectCallback; // 回调函数，执行上层逻辑，回调函数的参数由下层传入

    // 新连接信息，accept 后保存，供上层通过接口获取（思考：和回调参数传 connFd/peerAddr 哪种方式更好？）
    int acceptedConnFd;           // 最近 accept 的连接 fd
    InetAddress acceptedPeerAddr; // 最近 accept 的对端地址

public:
    Acceptor(EventLoop* _loop, const InetAddress& _listenAddr);
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    ~Acceptor();

    int get_listen_fd() const;
    void set_connect_callback(NewConnectCallback cb);

    // 获取最近 accept 的连接信息（在 newConnectCallback 回调中使用）
    int get_accepted_fd();
    const InetAddress& get_accepted_peer_addr();

private:
    int create_fd();
    void bind_address(int listenFd);
    void start_listen(int listenFd);

    // 理论上 Acceptor 不会触发 error、close、write 事件，只监听读事件（新连接到来）。但为了完整性，仍然预留这些回调接口处理逻辑
    void on_error(Channel& channel);
    void on_close(Channel& channel);
    void on_write(Channel& channel);
    void on_read(Channel& channel); // 有新连接到来，循环 accept

    void handle_connect_callback(); // 触发上层回调
};
