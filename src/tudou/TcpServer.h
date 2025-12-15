/**
 * @file TcpServer.h
 * @brief TCP 服务器：管理 Acceptor 与 TcpConnection
 * @author WenXingming
 * @project: https://github.com/WenXingming/Tudou
 * @details
 *
 * - 持有 Acceptor，监听并接受新连接，在回调中创建/接管 TcpConnection。
 * - 维护 fd->TcpConnection 的映射，负责连接增删及资源回收。
 */

#pragma once
#include <map>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>

#include "../base/InetAddress.h"
#include "EventLoopThreadPool.h"

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
    std::unique_ptr<EventLoopThreadPool> loopThreadPool; // 包括 1 个 mainLoop 和 多个 ioLoops

    std::string ip;
    uint16_t port;
    std::unique_ptr<Acceptor> acceptor;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections; // 生命期、持有者模糊，所以用 shared_ptr
    std::mutex connectionsMutex; // 保护 connections，在多线程 IO loop 场景下避免数据竞争
    // std::mutex connectionsMutex; // 保护 connections，在多线程 IO loop 场景下避免数据竞争

    ConnectionCallback connectionCallback;
    MessageCallback messageCallback;
    CloseCallback closeCallback;
    /// TODO: WriteCompleteCallback writeCompleteCallback;

public:
    TcpServer(std::string ip, uint16_t port, size_t ioLoopNum = 0);
    ~TcpServer();

    const std::string& get_ip() const { return ip; }
    uint16_t get_port() const { return port; }

    // TcpConnection 发布。不是 TcpServer 发布，Server 只是作为消息传递中间商
    void set_connection_callback(ConnectionCallback _cb);
    void set_message_callback(MessageCallback _cb);
    void set_close_callback(CloseCallback _cb);

    void start(); // 启动服务器，开始监听

    void send_message(int fd, const std::string& msg);

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
    // EventLoop* get_loop();
};
