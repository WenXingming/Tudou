// ============================================================================
// TcpServer 负责监听 TCP 端口、把新连接分配到 IO 线程，并转发连接事件。
// 它管理 TcpConnection 的创建、连接表和停止阶段的资源收口。
// ============================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "tudou/tcp/TcpConnection.h"

class Acceptor;
class EventLoop;
class EventLoopThreadPool;
class InetAddress;
class Socket;

class TcpServer {
public:
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback = std::function<void(const TcpConnectionPtr&)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
    using ErrorCallback = std::function<void(const TcpConnectionPtr&)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
    using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;

    TcpServer(std::string ip, uint16_t port, size_t ioLoopNum = 0);
    ~TcpServer();

    // 生命周期控制。
    void start();
    void stop();

    // 连接事件回调配置。
    void set_connection_callback(ConnectionCallback cb);
    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);
    void set_error_callback(ErrorCallback cb);
    void set_write_complete_callback(WriteCompleteCallback cb);
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark = 64 * 1024 * 1024);

    // 配置之后创建的连接的空闲检测策略。
    void set_connection_heartbeat(double checkIntervalSeconds, double idleTimeoutSeconds);

    // 服务器配置查询。
    const std::string& get_ip() const { return ip_; }
    uint16_t get_port() const { return port_; }
    int get_num_threads() const { return static_cast<int>(ioLoopNum_ + 1); }

private:
    // Acceptor 收到新连接后的装配流程。
    void on_connect(Socket connSocket, const InetAddress& peerAddr);
    TcpConnectionPtr create_connection(EventLoop& ioLoop,
        Socket connSocket,
        const InetAddress& peerAddr);

    // 连接事件处理及其辅助操作。
    void on_message(const TcpConnectionPtr& conn);
    void on_close(const TcpConnectionPtr& conn);
    void remove_connection(const TcpConnectionPtr& conn);

    // 主循环退出后的连接收口。
    void shutdown_connections();

private:
    std::string ip_;
    uint16_t port_;
    size_t ioLoopNum_;

    std::unique_ptr<EventLoopThreadPool> loopThreadPool_;

    std::unique_ptr<Acceptor> acceptor_;
    std::atomic<bool> accepting_;                    // 是否允许接收并创建新连接。避免在 stop() 过程中仍然创建新连接。

    // 外层 map 在 start() 阶段构建、线程池 join 后清理，运行期只读。
    // 每个内层 map 只由所属 EventLoop 线程访问。
    std::unordered_map<
        EventLoop*,
        std::unordered_map<TcpConnection*, TcpConnectionPtr>
    >connectionRecordsByLoop_;
    double heartbeatCheckIntervalSeconds_;          // 每个连接的心跳检测间隔，单位秒。<=0 表示禁用心跳检测。
    double heartbeatIdleTimeoutSeconds_;            // 每个连接的心跳空闲超时，单位秒。<=0 表示禁用心跳检测。

    std::atomic<size_t> activeConnectionCount_;     // 停止时等待所有连接完成收口。
    std::mutex shutdownMutex_;                      // 仅用于条件变量等待。
    std::condition_variable shutdownCondition_;     // 等待连接计数归零。

    ConnectionCallback connectionCallback_;         // 业务回调
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    ErrorCallback errorCallback_;
    WriteCompleteCallback writeCompleteCallback_;

    size_t highWaterMark_;
    HighWaterMarkCallback highWaterMarkCallback_;
};
