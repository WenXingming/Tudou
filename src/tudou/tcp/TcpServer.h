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

#include "base/InetAddress.h"
#include "EventLoopThreadPool.h"

class EventLoop;
class Acceptor;
class TcpConnection;
class Buffer;
class InetAddress;

class TcpServer {
    // 参数设计：上层使用下层，所以参数是下层类型，因为一般通过 composition 来使用下层类，参数一般是指针或引用
    // 使用 shared_ptr<TcpConnection> 作为回调参数，让业务层能够直接访问连接对象，获取更多信息（如对端地址、发送数据等）
    // 使用 shared_ptr 保证回调过程中对象不会被析构，同时让业务层可以按需保存连接对象
    // MessageCallback 不再传递 msg 参数，业务层通过 conn->receive() 主动获取数据，职责更清晰，也避免不必要的字符串拷贝
    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using ErrorCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using WriteCompleteCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using HighWaterMarkCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

private:
    std::unique_ptr<EventLoopThreadPool> loopThreadPool; // 包括 1 个 mainLoop（和多个 ioLoops）

    std::string ip;
    uint16_t port;
    std::unique_ptr<Acceptor> acceptor;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections; // 生命期、持有者模糊，所以用 shared_ptr（也有一个原因是为了共享访问，确保访问安全即访问过程中的对象不会被提前析构）
    std::mutex connectionsMutex; // 保护 connections，在多线程 IO loop 场景下避免数据竞争

    ConnectionCallback connectionCallback;
    MessageCallback messageCallback;
    CloseCallback closeCallback;
    ErrorCallback errorCallback;
    WriteCompleteCallback writeCompleteCallback;
    HighWaterMarkCallback highWaterMarkCallback;
    size_t highWaterMark;  // 高水位标记，单位字节，默认 64MB

public:
    TcpServer(std::string _ip, uint16_t _port, size_t _ioLoopNum = 0);
    ~TcpServer();

    const std::string& get_ip() const { return ip; }
    uint16_t get_port() const { return port; }

    int get_num_threads() const { return loopThreadPool ? loopThreadPool->get_num_threads() : 0; }

    // 底层由 TcpConnection 触发
    void set_connection_callback(ConnectionCallback _cb);
    void set_message_callback(MessageCallback _cb);
    void set_close_callback(CloseCallback _cb);
    void set_error_callback(ErrorCallback _cb);
    void set_write_complete_callback(WriteCompleteCallback _cb);
    void set_high_water_mark_callback(HighWaterMarkCallback _cb, size_t _highWaterMark = 64 * 1024 * 1024);

    // 启动服务器，开始监听
    void start();

private:
    // Acceptor 的回调处理函数，参数是 Acceptor 引用。处理新连接逻辑
    // 通过 acceptor.get_accepted_fd() 和 acceptor.get_accepted_peer_addr() 获取连接信息
    // TcpConnection 的回调函数。处理消息解析、连接关闭等逻辑
    void on_connect(Acceptor& acceptor);
    void on_message(const std::shared_ptr<TcpConnection>& conn);
    void on_close(const std::shared_ptr<TcpConnection>& conn);

    void handle_connection_callback(const std::shared_ptr<TcpConnection>& conn);
    void handle_message_callback(const std::shared_ptr<TcpConnection>& conn);
    void handle_close_callback(const std::shared_ptr<TcpConnection>& conn);
    void handle_error_callback(const std::shared_ptr<TcpConnection>& conn);
    void handle_write_complete_callback(const std::shared_ptr<TcpConnection>& conn);
    void handle_high_water_mark_callback(const std::shared_ptr<TcpConnection>& conn);

    void remove_connection(const std::shared_ptr<TcpConnection>& conn);

    void assert_in_main_loop_thread() const;
    EventLoop* select_loop() const;
};
