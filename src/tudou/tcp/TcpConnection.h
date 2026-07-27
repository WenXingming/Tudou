// ============================================================================
// TcpConnection 负责单个 TCP 连接的字节收发和关闭。
// Socket 持有 fd，Channel 分发事件，所有连接状态均在所属 EventLoop 线程维护。
// ============================================================================

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "tudou/tcp/InetAddress.h"
#include "tudou/tcp/Buffer.h"
#include "tudou/reactor/Channel.h"
#include "tudou/tcp/Socket.h"

class EventLoop;
class ConnectionHeartbeat;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using MessageCallback = std::function<void(const TcpConnectionPtr&)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
    using ErrorCallback = std::function<void(const TcpConnectionPtr&)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
    using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&)>;

    // 创建连接并绑定 Channel；两个心跳参数都大于 0 时启用空闲检测。
    static TcpConnectionPtr create_connection(EventLoop* loop,
        Socket connSocket,
        const InetAddress& peerAddr,
        double heartbeatCheckIntervalSeconds = 0.0,
        double heartbeatIdleTimeoutSeconds = 0.0);

    ~TcpConnection();

    // 连接收发和主动关闭。
    void send(std::string msg);
    std::string receive();
    void force_close();

    // 事件回调配置。
    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);
    void set_error_callback(ErrorCallback cb);
    void set_write_complete_callback(WriteCompleteCallback cb);
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark);

    // 状态查询。
    EventLoop* get_loop() const { return loop_; }
    int get_fd() const { return connSocket_.fd(); }
    const InetAddress& get_peer_addr() const { return peerAddr_; }
    size_t get_write_buffer_size() const { return writeBuffer_.readable_bytes(); }

private:
    explicit TcpConnection(EventLoop* loop, Socket connSocket, const InetAddress& peerAddr);

    // 发送流程。
    void send_in_loop(const std::string& msg);

    // Channel 事件处理。
    void on_read();
    void handle_message_callback();
    void on_write();
    void handle_write_complete_callback();
    void on_close();
    void close_connection();
    void handle_close_callback();
    void on_error();
    void handle_error_callback();
    void handle_high_water_mark_callback();

private:
    EventLoop* loop_;                                   // 所属 EventLoop，所有回调均在此线程执行。

    Socket connSocket_;                                 // 连接 socket 的 RAII 句柄，析构时自动关闭 fd（必须在 channel_ 之前声明）
    Channel channel_;                                   // 连接 fd 对应的 Channel，负责 epoll 事件回调。

    InetAddress peerAddr_;                              // 对端地址快照。

    Buffer readBuffer_;                                 // 应用层读缓冲。
    Buffer writeBuffer_;                                // 应用层写缓冲。

    size_t highWaterMark_;                              // 发送缓冲高水位阈值（字节）。

    MessageCallback messageCallback_;                   // 消息到达时触发（必选）。
    CloseCallback closeCallback_;                       // 连接关闭时触发（必选）。
    ErrorCallback errorCallback_;                       // 读写错误时触发（可选）。
    WriteCompleteCallback writeCompleteCallback_;       // 写缓冲清空时触发（可选）。
    HighWaterMarkCallback highWaterMarkCallback_;       // 写缓冲越过高水位时触发（可选）。

    bool isClosed_;                                     // 是否已关闭，保证 close_connection 幂等。

    std::unique_ptr<ConnectionHeartbeat> heartbeat_;     // 可选的连接空闲检测器，生命周期跟随连接。智能指针根据传入参数延迟创建
};
