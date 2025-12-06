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
 * - on_connect(): 接受连接并创建 TcpConnection，注册读/写/关闭/错误回调。
 * - on_close()/remove_connection(): 从映射中移除并清理。
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

class TcpServer {
    // 参数设计：上层使用下层，所以参数是下层类型，因为一般通过 composition 来使用下层类，参数一般是指针或引用
    // 通常：using ConnectionCallback = std::function<void(const TcpServer&)>;
    // But：但是这里如果回调函数的参数是 TcpServer& 显然不行，因为上层业务层不需要 TcpServer 对象本身的信息，而是连接信息。1(TcpServer) vs n(Connection)，所以需要传递连接相关参数
    // 解释：之前使用的参数是 using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>，同时了避免回调过程中对象被析构。但是缺点是类之间的通信耦性不好，TcpServer 直接暴露了 TcpConnection 给上层业务逻辑（在我的设计里类之间只应在相邻层通信）
    using ConnectionCallback = std::function<void(int fd)>;
    using MessageCallback = std::function<void(int fd, const std::string& msg)>;
    using CloseCallback = std::function<void(int fd)>;

private:
    std::unique_ptr<EventLoop> loop; // IO 线程的事件循环。还有一种线程是业务线程，负责处理业务逻辑，该线程是多线程，还没有实现
    std::string ip;
    uint16_t port;
    std::unique_ptr<Acceptor> acceptor;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections; // 生命期模糊，用户也可以持有。所以用 shared_ptr

    ConnectionCallback connectionCallback{ nullptr };
    MessageCallback messageCallback{ nullptr };
    CloseCallback closeCallback{ nullptr };

public:
    TcpServer(std::string ip, uint16_t port);
    ~TcpServer();

    const std::string& get_ip() const { return ip; }
    uint16_t get_port() const { return port; }

    // TcpConnection 发布。不是 TcpServer 发布，Server 只是作为消息传递中间商
    void set_connection_callback(ConnectionCallback _cb);
    void set_message_callback(MessageCallback _cb);
    void set_close_callback(CloseCallback _cb);

    void start(); // 启动服务器，开始监听

    void send(int fd, const std::string& msg);

private:
    // Acceptor 的回调处理函数，参数不是 Acceptor&，而是 connFd。处理新连接逻辑
    // TcpConnection 的回调函数。处理消息解析、连接关闭等逻辑
    void on_connect(const int);
    void on_message(const std::shared_ptr<TcpConnection>& conn);
    void on_close(const std::shared_ptr<TcpConnection>& conn);

    void handle_connection_callback(int fd);
    void handle_message_callback(int fd, const std::string& msg);
    void handle_close_callback(int fd);

    void remove_connection(const std::shared_ptr<TcpConnection>& conn);
};
