/**
 * @file TcpServer.h
 * @brief TCP 服务器：管理 Acceptor 与 TcpConnection，会话创建、回调接线与连接生命周期管理。
 * @author
 * @project: https://github.com/WenXingming/tudou
 * @details
 *
 * 职责：
 * - 持有 Acceptor，监听并接受新连接，在回调中创建/接管 TcpConnection。
 * - 维护 fd->TcpConnection 的映射，负责连接增删及资源回收。
 * - 将业务层 MessageCallback 与 TcpConnection 的 message/close 回调进行接线与转发。
 *
 * 线程模型与约定：
 * - 与所属 EventLoop 线程绑定，非线程安全；所有对外方法应在该线程调用。
 *
 * 生命周期与所有权：
 * - 唯一拥有 Acceptor（std::unique_ptr）。
 * - 以 std::shared_ptr 管理 TcpConnection 的生命周期，允许业务层持有副本。
 * - 仅保存回调，不持有上层业务对象，避免环依赖。
 *
 * 运行流程：
 * - start(): 启动监听（Acceptor 注册读事件）。
 * - connect_callback(): 接受连接并创建 TcpConnection，注册读/写/关闭/错误回调。
 * - close_callback()/remove_connection(): 从映射中移除并清理。
 *
 * 错误处理：
 * - 接受新连接失败或资源不足时记录并忽略本次事件，保持服务可用。
 */

#pragma once
#include <map>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>

#include "../base/InetAddress.h"

class EventLoop;
class Acceptor;
class TcpConnection;
class Buffer;
class InetAddress;
// 前向声明只能用于指针/引用/函数参数/返回值 等“不需要知道对象大小”的场景，
// 但你在 TcpServer 里是直接按值持有一个 InetAddress listenAddr;，这就需要完整类型定义，所以仅有前向声明不够。
// 如果你想避免包含 InetAddress.h，可以改为持有指针或引用，例如：std::unique_ptr<InetAddress> listenAddr;

class TcpServer {
    // 上层使用下层，所以参数是下层类型，因为一般通过 composition 来使用下层类。参数一般是指针或引用
    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>; // 只需要 fd 即可，无需也最好不要传递 TcpConnection 对象

private:
    std::unique_ptr<EventLoop> loop;
    InetAddress listenAddr;
    std::unique_ptr<Acceptor> acceptor;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections; // 生命期模糊，用户也可以持有。所以用 shared_ptr

    ConnectionCallback connectionCallback{ nullptr };
    MessageCallback messageCallback{ nullptr };
    CloseCallback closeCallback{ nullptr };

public:
    TcpServer(std::string ip, uint16_t port);
    ~TcpServer();

    // TcpConnection 发布。不是 TcpServer 发布，Server 只是作为消息传递中间商
    void set_connection_callback(ConnectionCallback cb);
    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);

    void start(); // 启动服务器，开始监听

private:
    // Acceptor 的回调处理函数，参数不是 Acceptor&，而是 connFd。处理新连接逻辑
    void connect_callback(const int);

    // TcpConnection 的回调函数。处理消息解析、连接关闭等逻辑
    void message_callback(const std::shared_ptr<TcpConnection>& conn);
    void close_callback(const std::shared_ptr<TcpConnection>& conn);

    void handle_connection(const std::shared_ptr<TcpConnection>& conn);
    void handle_message(const std::shared_ptr<TcpConnection>& conn);
    void handle_close(const std::shared_ptr<TcpConnection>& conn);

    void remove_connection(const std::shared_ptr<TcpConnection>& conn);
};
