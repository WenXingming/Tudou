/**
 * @file Acceptor.h
 * @brief 监听新连接的接入器（封装 listenFd 及持有其 Channel），在有连接到来时接受并上报给上层。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
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
 * - 负责 listenFd 的管理；对已接受的 connFd 仅发布，不负责其后续生命周期（由上层 TcpConnection 管理）。
 *
 * 错误处理：
 * - accept 失败时记录日志并进行必要的失败分支处理（如 EAGAIN、资源耗尽等）。
 */

#pragma once
#include <functional>
#include <memory>

#include "../base/InetAddress.h"
#include "../base/NonCopyable.h"

class EventLoop;
class Channel;
class InetAddress;
class Acceptor : public NonCopyable {
    // 上层使用下层，所以参数是下层类型，因为一般通过 composition 来使用下层类。参数一般是指针或引用
    // using ConnectCallback = std::function<void(const Acceptor&)>;
    // 直接传递 connFd 更简单，因为上层只需要这个 fd 来创建 TcpConnection。Acceptor 只能提供 listenFd，没有 connFd
    using ConnectCallback = std::function<void(int)>;

private:
    EventLoop* loop;
    int listenFd;
    InetAddress listenAddr;
    std::unique_ptr<Channel> channel;
    ConnectCallback connectCallback{ nullptr }; // 回调函数，执行上层逻辑，回调函数的参数由下层传入

public:
    Acceptor(EventLoop* _loop, const InetAddress& _listenAddr);
    ~Acceptor();

    int get_listen_fd() const;
    void set_connect_callback(std::function<void(int)> cb);

private:
    void create_fd();
    void bind_address();
    void start_listen();

    // 理论上 Acceptor 不会触发 error、close、write 事件，只监听读事件（新连接到来）。但为了完整性，仍然预留这些回调接口处理逻辑
    void error_callback();
    void close_callback();
    void write_callback();
    void read_callback();

    void handle_connect(int connFd); // 触发上层回调
};
