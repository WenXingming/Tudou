// ============================================================================
// TcpServer.h
// TCP 服务器连接编排器，负责接收新连接、装配 TcpConnection，并向上转发会话事件。
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

    /**
     * @brief 启动 IO 线程池并进入主事件循环。
     * @post 主线程开始监听并接收新的 TCP 连接。
     */
    void start();

    /**
     * @brief 设置连接建立后的业务回调。
     * @param cb 连接建立时触发的回调。
     */
    void set_connection_callback(ConnectionCallback cb);

    /**
     * @brief 设置收到消息后的业务回调。
     * @param cb 消息到达时触发的回调。
     */
    void set_message_callback(MessageCallback cb);

    /**
     * @brief 设置连接关闭后的业务回调。
     * @param cb 连接关闭时触发的回调。
     */
    void set_close_callback(CloseCallback cb);

    /**
     * @brief 设置连接错误后的业务回调。
     * @param cb 连接发生错误时触发的回调。
     */
    void set_error_callback(ErrorCallback cb);

    /**
     * @brief 设置写缓冲完全刷入内核后的回调。
     * @param cb 写完成时触发的回调。
     */
    void set_write_complete_callback(WriteCompleteCallback cb);

    /**
     * @brief 设置高水位回调与阈值。
     * @param cb 积压超过阈值时触发的回调。
     * @param highWaterMark 高水位阈值，单位字节。
     */
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark = 64 * 1024 * 1024);

    /**
     * @brief 获取监听 IP。
     * @return 服务监听 IP。
     */
    const std::string& get_ip() const { return ip_; }

    /**
     * @brief 获取监听端口。
     * @return 服务监听端口。
     */
    uint16_t get_port() const { return port_; }

    /**
     * @brief 获取线程池中的 IO 线程数量。
     * @return IO 线程数量。
     */
    int get_num_threads() const { return loopThreadPool_ ? loopThreadPool_->get_num_threads() : 0; }

private:
    /**
     * @brief 获取主事件循环；若主循环缺失则终止程序。
     * @return 主事件循环引用。
     */
    EventLoop& require_main_loop() const;

    /**
     * @brief 校验当前线程就是主事件循环线程。
     */
    void assert_in_main_loop_thread() const;

    /**
     * @brief 选择一个负责该连接的 IO 事件循环。
     * @return 被选中的 IO 事件循环引用。
     */
    EventLoop& select_loop() const;

    /**
     * @brief 处理 acceptor 上报的新连接，并在目标线程中完成装配。
     * @param connFd 新连接 fd。
     * @param peerAddr 对端地址。
     */
    void on_connect(int connFd, const InetAddress& peerAddr);

    /**
     * @brief 读取刚建立连接的本地地址。
     * @param connFd 新连接 fd。
     * @return 本地地址；失败时返回零值地址。
     */
    InetAddress resolve_local_address(int connFd) const;

    /**
     * @brief 创建一个 TcpConnection 对象并绑定所属 IO 线程。
     * @param ioLoop 负责该连接的 IO 事件循环。
     * @param connFd 新连接 fd。
     * @param localAddr 本地地址。
     * @param peerAddr 对端地址。
     * @return 新建的连接对象。
     */
    std::shared_ptr<TcpConnection> create_connection(EventLoop& ioLoop,
        int connFd,
        const InetAddress& localAddr,
        const InetAddress& peerAddr) const;

    /**
     * @brief 配置连接的传输层选项。
     * @param conn 待配置的连接对象。
     */
    void configure_connection_socket(const std::shared_ptr<TcpConnection>& conn) const;

    /**
     * @brief 为新连接绑定 TcpServer 侧的事件回调。
     * @param conn 待绑定回调的连接对象。
     */
    void bind_connection_callbacks(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 将连接注册到 fd->connection 映射中。
     * @param connFd 新连接 fd。
     * @param conn 待注册的连接对象。
     */
    void store_connection(int connFd, const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 完成连接自保活与 Channel 绑定。
     * @param conn 已装配完成的连接对象。
     */
    void establish_connection(const std::shared_ptr<TcpConnection>& conn) const;

    /**
     * @brief 向上分发连接建立事件。
     * @param conn 已建立的连接对象。
     */
    void notify_connection_callback(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 转发连接上的消息事件。
     * @param conn 触发消息事件的连接对象。
     */
    void on_message(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 向上分发消息事件。
     * @param conn 触发消息事件的连接对象。
     */
    void notify_message_callback(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 处理连接关闭事件。
     * @param conn 已关闭的连接对象。
     */
    void on_close(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 从连接表中移除一个连接。
     * @param conn 待移除的连接对象。
     */
    void remove_connection(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 向上分发连接关闭事件。
     * @param conn 已关闭的连接对象。
     */
    void notify_close_callback(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 向上分发连接错误事件。
     * @param conn 发生错误的连接对象。
     */
    void notify_error_callback(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 向上分发写完成事件。
     * @param conn 写缓冲已刷空的连接对象。
     */
    void notify_write_complete_callback(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 向上分发高水位事件。
     * @param conn 写缓冲超过阈值的连接对象。
     */
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
