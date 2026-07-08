// ============================================================================
// TcpServer.h
// TCP 服务器连接编排器，负责接收新连接、装配 TcpConnection，并向上转发会话事件。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// TcpServer.h
// └── TcpServer
//     ├── TcpServer(ip, port, ioLoopNum)         # [公有] 构造：仅记录配置，不创建任何运行时资源
//     ├── ~TcpServer()                            # [公有] 析构：资源由成员对象统一回收
//     ├── start()                                 # [公有] 启动线程池、创建 Acceptor 并进入主事件循环
//     │   └── on_connect(connSocket, peerAddr)    # [私有] 绑定为 Acceptor 的新连接回调
//     │       └── create_connection(...)          # [私有] 创建 TcpConnection、配置 socket 选项、绑定回调、存入所属 loop 连接表
//     │           ├── create_connection_heartbeat(conn) const # [私有] 按需实例化空闲检测策略对象
//     │           ├── on_message(conn)            # [私有] 消息事件先刷新心跳再向上转发
//     │           │   └── refresh_connection_heartbeat(conn) # [私有] 更新连接最后活跃时间
//     │           └── on_close(conn)              # [私有] 关闭主干：先删连接，再通知业务层
//     │               └── remove_connection(conn) # [私有] 从连接表中移除并停止该连接的心跳定时器
//     │   └── shutdown_connections()              # [私有] 退出主循环后主动收口剩余连接，再销毁线程绑定资源
//     ├── stop()                                  # [公有] 请求主 EventLoop 退出
//     ├── set_connection_callback(cb)             # [公有] 注册建连回调
//     ├── set_message_callback(cb)                # [公有] 注册消息回调
//     ├── set_close_callback(cb)                  # [公有] 注册关闭回调
//     ├── set_error_callback(cb)                  # [公有] 注册错误回调
//     ├── set_write_complete_callback(cb)         # [公有] 注册写完成回调
//     ├── set_high_water_mark_callback(cb, mark)  # [公有] 注册高水位回调并设置阈值
//     ├── set_connection_heartbeat(interval, timeout) # [公有] 配置所有连接共享的空闲检测策略
//     ├── get_ip() const                          # [公有] 返回监听 IP
//     ├── get_port() const                        # [公有] 返回监听端口
//     └── get_num_threads() const                 # [公有] 返回线程池 loop 总数
// ============================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "tudou/tcp/TcpConnection.h"

class Acceptor;
class ConnectionHeartbeat;
class EventLoop;
class EventLoopThreadPool;
class InetAddress;
class Socket;

// TcpServer 是 TCP 层的门面，负责把"接收连接"压平成"选择线程、装配连接、注册连接、通知上层"的单向流程。
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

    void start();
    void stop();

    void set_connection_callback(ConnectionCallback cb);
    void set_message_callback(MessageCallback cb); // 回调如需消费本次读到的数据，可自行调用 conn->receive()。
    void set_close_callback(CloseCallback cb);
    void set_error_callback(ErrorCallback cb);
    void set_write_complete_callback(WriteCompleteCallback cb);
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark = 64 * 1024 * 1024);
    // 为之后创建的所有连接启用统一的空闲检测；策略对象的生命周期由 TcpServer 统一编排。
    void set_connection_heartbeat(double checkIntervalSeconds, double idleTimeoutSeconds);
    // 启用或禁用 CPU 亲和性设置
    void enable_cpu_affinity(bool enable = true) { pinCpu_ = enable; }
    const std::string& get_ip() const { return ip_; }
    uint16_t get_port() const { return port_; }
    int get_num_threads() const { return static_cast<int>(ioLoopNum_ + 1); }

private:
    enum class ServerState {
        Created,
        Running,
        Draining,
        Stopped
    };

    struct ConnectionRecord {
        TcpConnectionPtr connection;
        std::shared_ptr<ConnectionHeartbeat> heartbeat;
    };

    struct ConnectionHeartbeatOptions {
        bool enabled = false;
        double checkIntervalSeconds = 0.0;
        double idleTimeoutSeconds = 0.0;
    };

    using ConnectionRecords = std::unordered_map<TcpConnection*, ConnectionRecord>;

    // 新连接装配总入口，接收 Socket 所有权。
    void on_connect(Socket connSocket, const InetAddress& peerAddr);
    void on_message(const TcpConnectionPtr& conn);
    void on_close(const TcpConnectionPtr& conn);

    TcpConnectionPtr create_connection(EventLoop& ioLoop,
        Socket connSocket,
        const InetAddress& peerAddr);
    void remove_connection(const TcpConnectionPtr& conn);

    std::shared_ptr<ConnectionHeartbeat> create_connection_heartbeat(const TcpConnectionPtr& conn) const;
    void refresh_connection_heartbeat(const TcpConnectionPtr& conn);
    void shutdown_connections();

private:
    std::unique_ptr<EventLoopThreadPool> loopThreadPool_;
    size_t ioLoopNum_;

    std::string ip_;
    uint16_t port_;
    std::unique_ptr<Acceptor> acceptor_;

    // 【无锁/分片设计】
    // 外层哈希(EventLoop*)：在 start() 阶段一次性初始化完毕，运行期为纯只读结构，多线程并发查找（find）天然安全。
    // 内层哈希(ConnectionRecords)：严格遵守 Thread-Per-Core 原则，只有该 loop 所属线程才有权执行增删改查。因此全程无锁。
    std::unordered_map<EventLoop*, ConnectionRecords> connectionRecordsByLoop_;
    std::atomic<size_t> activeConnectionCount_;     // 只用于 shutdown 触发所有连接关闭后同步等待所有连接销毁完成
    std::mutex shutdownMutex_;                      // 保护 shutdownCondition_ 的 wait/notify 握手，不保护 activeConnectionCount_ 本身
    std::condition_variable shutdownCondition_;     // 替代 busy-wait，由 shutdown lambda 在计数归零时唤醒主线程
    std::atomic<ServerState> state_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    ErrorCallback errorCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;

    size_t highWaterMark_;
    ConnectionHeartbeatOptions connectionHeartbeatOptions_;
    bool pinCpu_ = false;                                               // 是否开启 CPU 亲和性。
};
