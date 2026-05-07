// ============================================================================
// TcpServer.h
// TCP 服务器连接编排器，负责接收新连接、装配 TcpConnection，并向上转发会话事件。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// TcpServer.h
// └── TcpServer
//     ├── TcpServer(ip, port, ioLoopNum)         # [公有] 构造：创建线程池、主 loop 和 Acceptor
//     │   ├── require_main_loop() const          # [私有] 取出主 EventLoop 作为监听线程
//     │   └── on_connect(connFd, peerAddr)       # [私有] 绑定为 Acceptor 的新连接回调
//     │       ├── assert_in_main_loop_thread() const  # [私有] 确保 accept 回调在主 loop 线程执行
//     │       ├── select_loop() const                 # [私有] 轮询选出负责该连接的 IO loop
//     │       ├── resolve_local_address(connFd) const # [私有] 读取本地地址快照
//     │       ├── create_connection(...) const        # [私有] 构造 TcpConnection
//     │       ├── configure_connection_socket(conn) const  # [私有] 统一配置 no-delay/keepalive
//     │       ├── bind_connection_callbacks(conn)     # [私有] 绑定消息、关闭、错误、背压等回调
//     │       │   ├── on_message(conn)                # [私有] TcpServer 只转发消息事件
//     │       │   │   └── notify_message_callback(conn) # [私有] 触发上层 messageCallback_
//     │       │   ├── on_close(conn)                  # [私有] 关闭主干：先删连接，再通知业务层
//     │       │   │   ├── remove_connection(conn)     # [私有] 从 connections_ 中移除 fd
//     │       │   │   └── notify_close_callback(conn) # [私有] 转发连接关闭事件
//     │       │   ├── notify_error_callback(conn)     # [私有] 向上分发错误事件
//     │       │   ├── notify_write_complete_callback(conn)  # [私有] 向上分发写完成事件
//     │       │   └── notify_high_water_mark_callback(conn) # [私有] 向上分发高水位事件
//     │       ├── store_connection(connFd, conn)      # [私有] 放入活动连接表
//     │       ├── establish_connection(conn) const    # [私有] 触发 tie/self-keepalive 初始化
//     │       └── notify_connection_callback(conn)    # [私有] 把“连接已建立”事件抛给业务层
//     ├── ~TcpServer()                            # [公有] 析构：资源由成员对象统一回收
//     ├── start()                                 # [公有] 启动 IO 线程池并进入主事件循环
//     │   └── require_main_loop() const           # [私有] 获取主 loop
//     ├── set_connection_callback(cb)             # [公有] 注册建连回调
//     ├── set_message_callback(cb)                # [公有] 注册消息回调
//     ├── set_close_callback(cb)                  # [公有] 注册关闭回调
//     ├── set_error_callback(cb)                  # [公有] 注册错误回调
//     ├── set_write_complete_callback(cb)         # [公有] 注册写完成回调
//     ├── set_high_water_mark_callback(cb, mark)  # [公有] 注册高水位回调并设置阈值
//     ├── get_ip() const                          # [公有] 返回监听 IP
//     ├── get_port() const                        # [公有] 返回监听端口
//     └── get_num_threads() const                 # [公有] 返回线程池 loop 总数
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "base/InetAddress.h"
#include "EventLoopThreadPool.h"

class Acceptor;
class EventLoop;
class TcpConnection;

// TcpServer 是 TCP 层的门面，负责把“接收连接”压平成“选择线程、装配连接、注册连接、通知上层”的单向流程。
class TcpServer {
public:
    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using ErrorCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using WriteCompleteCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using HighWaterMarkCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    TcpServer(std::string ip, uint16_t port, size_t ioLoopNum = 0);
    ~TcpServer();

    void start(); // 启动 IO 线程池，并让主 loop 开始接收连接。

    void set_connection_callback(ConnectionCallback cb);
    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);
    void set_error_callback(ErrorCallback cb);
    void set_write_complete_callback(WriteCompleteCallback cb);
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark = 64 * 1024 * 1024);
    const std::string& get_ip() const { return ip_; }
    uint16_t get_port() const { return port_; }
    int get_num_threads() const { return loopThreadPool_ ? loopThreadPool_->get_num_threads() : 0; }

private:
    EventLoop& require_main_loop() const;
    void assert_in_main_loop_thread() const;
    EventLoop& select_loop() const;
    void on_connect(int connFd, const InetAddress& peerAddr); // 新连接装配总入口。
    InetAddress resolve_local_address(int connFd) const;
    std::shared_ptr<TcpConnection> create_connection(EventLoop& ioLoop,
        int connFd,
        const InetAddress& localAddr,
        const InetAddress& peerAddr) const;
    void configure_connection_socket(const std::shared_ptr<TcpConnection>& conn) const;
    void bind_connection_callbacks(const std::shared_ptr<TcpConnection>& conn); // 接回消息、关闭、错误等事件。
    void store_connection(int connFd, const std::shared_ptr<TcpConnection>& conn);
    void establish_connection(const std::shared_ptr<TcpConnection>& conn) const;
    void notify_connection_callback(const std::shared_ptr<TcpConnection>& conn);
    void on_message(const std::shared_ptr<TcpConnection>& conn);
    void notify_message_callback(const std::shared_ptr<TcpConnection>& conn);
    void on_close(const std::shared_ptr<TcpConnection>& conn);
    void remove_connection(const std::shared_ptr<TcpConnection>& conn); // 从活动连接表中删除 fd。
    void notify_close_callback(const std::shared_ptr<TcpConnection>& conn);
    void notify_error_callback(const std::shared_ptr<TcpConnection>& conn);
    void notify_write_complete_callback(const std::shared_ptr<TcpConnection>& conn);
    void notify_high_water_mark_callback(const std::shared_ptr<TcpConnection>& conn);

private:
    std::unique_ptr<EventLoopThreadPool> loopThreadPool_; // 拥有主事件循环与 IO 线程池。
    std::string ip_; // 监听 IP，由服务实例在整个生命周期内持有。
    uint16_t port_; // 监听端口，与 ip_ 共同定义监听地址。
    std::unique_ptr<Acceptor> acceptor_; // 负责监听 socket accept 的底层接入器。
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_; // 按 fd 持有活动连接，确保回调期间对象稳定存活。
    std::mutex connectionsMutex_; // 保护连接表，避免多 IO 线程并发增删时发生数据竞争。
    ConnectionCallback connectionCallback_; // 上层连接建立回调。
    MessageCallback messageCallback_; // 上层消息回调。
    CloseCallback closeCallback_; // 上层关闭回调。
    ErrorCallback errorCallback_; // 上层错误回调。
    WriteCompleteCallback writeCompleteCallback_; // 上层写完成回调。
    HighWaterMarkCallback highWaterMarkCallback_; // 上层高水位回调。
    size_t highWaterMark_; // 写缓冲高水位阈值，单位字节。
};
