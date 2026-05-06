// ============================================================================
// TcpConnection.h
// TcpConnection 负责单个连接的收发、关闭和心跳流程，并把底层事件压平成可读的业务步骤。
// ============================================================================

#pragma once

#include <cstddef>
#include <memory>
#include <functional>
#include <string>
#include <chrono>

#include "base/InetAddress.h"
#include "Channel.h"
#include "Buffer.h"
#include "Timer.h"

class EventLoop;

// TcpConnection 是面向连接的会话门面，负责维护连接状态并把 I/O 事件转换成稳定的上层回调。
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using ErrorCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using WriteCompleteCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using HighWaterMarkCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    TcpConnection(EventLoop* loop, int connFd, const InetAddress& localAddr, const InetAddress& peerAddr);
    ~TcpConnection();

    /**
     * @brief 把应用层数据追加到发送缓冲并注册写事件。
     * @param msg 待发送的应用层数据。
     */
    void send(const std::string& msg);

    /**
     * @brief 取走当前读缓冲中的全部数据。
     * @return 当前已接收但尚未消费的数据。
     */
    std::string receive();

    /**
     * @brief 设置消息回调。
     * @param cb 收到数据时触发的回调。
     */
    void set_message_callback(MessageCallback cb);

    /**
     * @brief 设置关闭回调。
     * @param cb 连接进入关闭流程时触发的回调。
     */
    void set_close_callback(CloseCallback cb);

    /**
     * @brief 设置错误回调。
     * @param cb 发生套接字错误时触发的回调。
     */
    void set_error_callback(ErrorCallback cb);

    /**
     * @brief 设置写完成回调。
     * @param cb 发送缓冲完全刷空时触发的回调。
     */
    void set_write_complete_callback(WriteCompleteCallback cb);

    /**
     * @brief 设置高水位回调。
     * @param cb 写缓冲积压越过阈值时触发的回调。
     * @param highWaterMark 高水位阈值，单位字节。
     */
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark);

    /**
     * @brief 建立 shared_from_this 保护并补启已配置的心跳任务。
     */
    void connection_establish();

    /**
     * @brief 设置 TCP_NODELAY。
     * @param on 为 true 时禁用 Nagle 算法，为 false 时恢复 Nagle。
     */
    void set_tcp_no_delay(bool on);

    /**
     * @brief 设置 TCP keepalive。
     * @param on 为 true 时启用 keepalive，并同步常用探测参数。
     */
    void set_tcp_keepalive(bool on);

    /**
     * @brief 启用应用层心跳。
     * @param intervalSeconds 心跳发送周期，单位秒。
     * @param timeoutSeconds 入站空闲超时阈值，单位秒。
     * @param pingMessage 周期发送的探测报文；空字符串表示只做超时检测。
     */
    void enable_app_heartbeat(double intervalSeconds, double timeoutSeconds, const std::string& pingMessage = "PING\r\n");

    /**
     * @brief 关闭应用层心跳并取消内部定时器。
     */
    void disable_app_heartbeat();

    /**
     * @brief 判断应用层心跳是否已启用。
     * @return true 表示心跳已启用。
     */
    bool is_app_heartbeat_enabled() const { return heartbeatEnabled_; }

    /**
     * @brief 获取所属事件循环。
     * @return 连接所属的 EventLoop 指针。
     */
    EventLoop* get_loop() const { return loop_; }

    /**
     * @brief 获取连接 fd。
     * @return 连接 fd。
     */
    int get_fd() const { return channel_->get_fd(); }

    /**
     * @brief 获取本地地址。
     * @return 本地地址引用。
     */
    const InetAddress& get_local_addr() const { return localAddr_; }

    /**
     * @brief 获取对端地址。
     * @return 对端地址引用。
     */
    const InetAddress& get_peer_addr() const { return peerAddr_; }

    /**
     * @brief 获取最近一次错误码。
     * @return 最近一次 errno。
     */
    int get_last_error() const { return lastErrorCode_; }

    /**
     * @brief 获取最近一次错误描述。
     * @return 最近一次错误描述字符串。
     */
    const std::string& get_last_error_msg() const { return lastErrorMsg_; }

    /**
     * @brief 获取写缓冲中的可读字节数。
     * @return 当前写缓冲积压字节数。
     */
    size_t get_write_buffer_size() const { return writeBuffer_->readable_bytes(); }

    /**
     * @brief 获取高水位阈值。
     * @return 当前高水位阈值，单位字节。
     */
    size_t get_high_water_mark() const { return highWaterMark_; }

private:
    /**
     * @brief 处理可读事件。
     * @param channel 触发事件的 Channel。
     */
    void on_read(Channel& channel);

    /**
     * @brief 从套接字读取数据到读缓冲。
     * @param channel 触发可读事件的 Channel。
     * @param savedErrno 输出参数，返回系统错误码。
     * @return 本次读取的字节数；0 表示对端关闭；负值表示出错。
     */
    ssize_t read_from_channel(const Channel& channel, int* savedErrno);

    /**
     * @brief 刷新“最近一次收到数据”的时间戳。
     */
    void refresh_last_read_time();

    /**
     * @brief 向上分发消息回调。
     */
    void notify_message_callback();

    /**
     * @brief 处理可写事件。
     * @param channel 触发事件的 Channel。
     */
    void on_write(Channel& channel);

    /**
     * @brief 把发送缓冲中的数据刷入套接字。
     * @param channel 触发可写事件的 Channel。
     * @param savedErrno 输出参数，返回系统错误码。
     * @return 本次写出的字节数；负值表示出错。
     */
    ssize_t write_to_channel(const Channel& channel, int* savedErrno);

    /**
     * @brief 向上分发写完成回调。
     */
    void notify_write_complete_callback();

    /**
     * @brief 处理关闭事件。
     * @param channel 触发事件的 Channel。
     */
    void on_close(Channel& channel);

    /**
     * @brief 执行一次幂等的关闭流程。
     * @param channel 当前连接绑定的 Channel。
     */
    void close_connection(Channel& channel);

    /**
     * @brief 向上分发关闭回调。
     */
    void notify_close_callback();

    /**
     * @brief 处理错误事件。
     * @param channel 触发事件的 Channel。
     */
    void on_error(Channel& channel);

    /**
     * @brief 记录最近一次套接字错误。
     * @param action 出错动作，例如 read/write。
     * @param errorCode 系统错误码。
     */
    void record_socket_error(const char* action, int errorCode);

    /**
     * @brief 向上分发错误回调。
     */
    void notify_error_callback();

    /**
     * @brief 向上分发高水位回调。
     */
    void notify_high_water_mark_callback();

    /**
     * @brief 把当前对象绑定到 Channel 的 tie 机制上。
     */
    void tie_channel_to_owner();

    /**
     * @brief 创建或重建周期性心跳定时器。
     */
    void start_app_heartbeat_timer();

    /**
     * @brief 停止当前心跳定时器。
     */
    void stop_app_heartbeat_timer();

    /**
     * @brief 执行一次心跳 tick，判断超时并按需发送探测报文。
     */
    void on_heartbeat_tick();

    /**
     * @brief 判断连接是否已经超过心跳超时阈值。
     * @param now 当前时间点。
     * @return true 表示连接已超时。
     */
    bool is_heartbeat_timeout(std::chrono::steady_clock::time_point now) const;

private:
    EventLoop* loop_; // 所属事件循环，非 owning，定义连接的线程归属。
    std::unique_ptr<Channel> channel_; // 连接 fd 对应的事件通道，由 TcpConnection 独占。
    InetAddress localAddr_; // 本地地址快照，用于诊断和上层查询。
    InetAddress peerAddr_; // 对端地址快照，用于业务层识别客户端。
    size_t highWaterMark_; // 写缓冲高水位阈值，单位字节。
    std::unique_ptr<Buffer> readBuffer_; // 入站数据缓冲，由 on_read 写入、receive 读取。
    std::unique_ptr<Buffer> writeBuffer_; // 出站数据缓冲，由 send 追加、on_write 刷入内核。
    MessageCallback messageCallback_; // 收到应用层数据时触发的回调。
    CloseCallback closeCallback_; // 连接关闭时触发的回调。
    ErrorCallback errorCallback_; // 套接字错误时触发的回调。
    WriteCompleteCallback writeCompleteCallback_; // 写缓冲清空时触发的回调。
    HighWaterMarkCallback highWaterMarkCallback_; // 写缓冲首次越过高水位时触发的回调。
    int lastErrorCode_; // 最近一次记录的 errno。
    std::string lastErrorMsg_; // 最近一次记录的错误描述。
    bool isClosed_; // 关闭流程是否已经执行过，防止多路径重复清理。
    bool heartbeatEnabled_; // 应用层心跳是否处于启用状态。
    double heartbeatIntervalSeconds_; // 心跳发送周期，单位秒。
    double heartbeatTimeoutSeconds_; // 空闲超时阈值，单位秒。
    std::string heartbeatPingMessage_; // 心跳探测报文内容。
    TimerId heartbeatTimerId_; // 当前心跳定时器 ID，用于取消旧任务。
    std::chrono::steady_clock::time_point lastReadTime_; // 最近一次收到数据的时间点。
};
