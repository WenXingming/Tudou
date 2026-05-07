// ============================================================================
// TcpConnection.h
// TcpConnection 负责单个连接的收发、关闭和心跳流程，并把底层事件压平成可读的业务步骤。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// TcpConnection.h
// └── TcpConnection
//     ├── TcpConnection(loop, connFd, localAddr, peerAddr)  # [公有] 构造：绑定 Channel 四类事件并默认开启读关注
//     │   ├── on_read(channel)                       # [私有] 绑定为读事件主干：读数据、判 EOF、判错误
//     │   │   ├── read_from_channel(channel, &err)   # [私有] 从 fd 搬运数据到 readBuffer_
//     │   │   ├── refresh_last_read_time()           # [私有] 有入站数据就刷新活跃时间
//     │   │   ├── notify_message_callback()          # [私有] 把“有消息”事件抛给上层
//     │   │   ├── record_socket_error("read", err)   # [私有] 记录读失败错误信息
//     │   │   ├── notify_error_callback()            # [私有] 通知上层错误
//     │   │   └── close_connection(channel)          # [私有] EOF 或致命错误都走统一关闭流程
//     │   │       ├── stop_app_heartbeat_timer()     # [私有] 避免关闭后残留定时器继续回调
//     │   │       └── notify_close_callback()        # [私有] 触发服务器侧连接移除逻辑
//     │   ├── on_write(channel)                      # [私有] 绑定为可写事件主干：刷发送缓冲并处理收尾
//     │   │   ├── write_to_channel(channel, &err)    # [私有] 把 writeBuffer_ 写入 fd
//     │   │   ├── notify_write_complete_callback()   # [私有] 写缓冲清空时通知上层
//     │   │   ├── record_socket_error("write", err)  # [私有] 记录写失败错误信息
//     │   │   ├── notify_error_callback()            # [私有] 通知上层错误
//     │   │   └── close_connection(channel)          # [私有] 致命写错误时统一收口
//     │   │       ├── stop_app_heartbeat_timer()     # [私有] 避免关闭后残留定时器继续回调
//     │   │       └── notify_close_callback()        # [私有] 触发服务器侧连接移除逻辑
//     │   ├── on_close(channel)                      # [私有] 绑定为 close 事件入口
//     │   │   └── close_connection(channel)          # [私有] 幂等关闭入口
//     │   │       ├── stop_app_heartbeat_timer()     # [私有] 避免关闭后残留定时器继续回调
//     │   │       └── notify_close_callback()        # [私有] 触发服务器侧连接移除逻辑
//     │   └── on_error(channel)                      # [私有] 绑定为 error 事件入口
//     │       ├── notify_error_callback()            # [私有] 向上转发错误事件
//     │       └── close_connection(channel)          # [私有] 与 read/write 错误共用收尾逻辑
//     │           ├── stop_app_heartbeat_timer()     # [私有] 避免关闭后残留定时器继续回调
//     │           └── notify_close_callback()        # [私有] 触发服务器侧连接移除逻辑
//     ├── ~TcpConnection()                       # [公有] 析构：连接对象生命周期结束点
//     ├── send(msg)                              # [公有] 统一发送入口，先入写缓冲再注册写事件
//     │   └── notify_high_water_mark_callback()  # [私有] 越过高水位阈值时上报背压
//     ├── receive()                              # [公有] 拉取并清空当前读缓冲中的应用层数据
//     ├── connection_establish()                 # [公有] 建立 shared_from_this 保活并补启心跳
//     │   ├── tie_channel_to_owner()             # [私有] 将自身绑到 Channel tie 机制
//     │   └── start_app_heartbeat_timer()        # [私有] 若已启用心跳则创建周期任务
//     │       ├── stop_app_heartbeat_timer()     # [私有] 先取消旧定时器避免重复触发
//     │       └── on_heartbeat_tick()            # [私有] run_every 到期后执行一次心跳 tick
//     │           ├── is_heartbeat_timeout(now) const    # [私有] 判断连接是否已空闲超时
//     │           ├── close_connection(*channel_)        # [私有] 超时则主动关闭连接
//     │           │   ├── stop_app_heartbeat_timer()     # [私有] 避免关闭后残留定时器继续回调
//     │           │   └── notify_close_callback()        # [私有] 触发服务器侧连接移除逻辑
//     │           └── send(heartbeatPingMessage_)        # [公有] 未超时则复用统一发送路径发探测包
//     ├── enable_app_heartbeat(interval, timeout, ping)  # [公有] 打开心跳并立即刷新活跃时间
//     │   ├── refresh_last_read_time()           # [私有] 重置最近收包时间
//     │   └── start_app_heartbeat_timer()        # [私有] 重建 run_every 定时器
//     │       ├── stop_app_heartbeat_timer()     # [私有] 先取消旧定时器避免重复触发
//     │       └── on_heartbeat_tick()            # [私有] run_every 到期后执行一次心跳 tick
//     │           ├── is_heartbeat_timeout(now) const    # [私有] 判断连接是否已空闲超时
//     │           ├── close_connection(*channel_)        # [私有] 超时则主动关闭连接
//     │           │   ├── stop_app_heartbeat_timer()     # [私有] 避免关闭后残留定时器继续回调
//     │           │   └── notify_close_callback()        # [私有] 触发服务器侧连接移除逻辑
//     │           └── send(heartbeatPingMessage_)        # [公有] 未超时则复用统一发送路径发探测包
//     ├── disable_app_heartbeat()                # [公有] 关闭心跳并取消定时器
//     │   └── stop_app_heartbeat_timer()         # [私有] cancel 当前 TimerId
//     ├── set_tcp_no_delay(on)                   # [公有] 配置 TCP_NODELAY
//     ├── set_tcp_keepalive(on)                  # [公有] 配置 SO_KEEPALIVE 与 TCP keepalive 参数
//     ├── set_message_callback(cb)               # [公有] 注册消息回调
//     ├── set_close_callback(cb)                 # [公有] 注册关闭回调
//     ├── set_error_callback(cb)                 # [公有] 注册错误回调
//     ├── set_write_complete_callback(cb)        # [公有] 注册写完成回调
//     ├── set_high_water_mark_callback(cb, mark) # [公有] 注册高水位回调并更新阈值
//     ├── is_app_heartbeat_enabled() const       # [公有] 查询心跳开关状态
//     ├── get_loop() const                       # [公有] 返回所属 EventLoop
//     ├── get_fd() const                         # [公有] 返回连接 fd
//     ├── get_local_addr() const                 # [公有] 返回本地地址快照
//     ├── get_peer_addr() const                  # [公有] 返回对端地址快照
//     ├── get_last_error() const                 # [公有] 返回最近错误码
//     ├── get_last_error_msg() const             # [公有] 返回最近错误描述
//     ├── get_write_buffer_size() const          # [公有] 返回当前发送积压字节数
//     └── get_high_water_mark() const            # [公有] 返回高水位阈值
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

    void send(const std::string& msg); // 统一发送入口：先进写缓冲，再由可写事件刷出。
    std::string receive(); // 取走当前读缓冲中的全部应用层数据。

    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);
    void set_error_callback(ErrorCallback cb);
    void set_write_complete_callback(WriteCompleteCallback cb);
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark);

    void connection_establish(); // 建立 Channel tie，并补启已经配置好的心跳。
    void set_tcp_no_delay(bool on);
    void set_tcp_keepalive(bool on);
    void enable_app_heartbeat(double intervalSeconds, double timeoutSeconds, const std::string& pingMessage = "PING\r\n"); // 开启应用层心跳检测。
    void disable_app_heartbeat();

    bool is_app_heartbeat_enabled() const { return heartbeatEnabled_; }
    EventLoop* get_loop() const { return loop_; }
    int get_fd() const { return channel_->get_fd(); }
    const InetAddress& get_local_addr() const { return localAddr_; }
    const InetAddress& get_peer_addr() const { return peerAddr_; }
    int get_last_error() const { return lastErrorCode_; }
    const std::string& get_last_error_msg() const { return lastErrorMsg_; }
    size_t get_write_buffer_size() const { return writeBuffer_->readable_bytes(); }
    size_t get_high_water_mark() const { return highWaterMark_; }

private:
    void on_read(Channel& channel); // 处理读事件、EOF 和致命读错误。
    ssize_t read_from_channel(const Channel& channel, int* savedErrno);
    void refresh_last_read_time();
    void notify_message_callback();
    void on_write(Channel& channel); // 刷发送缓冲并处理写完成或错误收口。
    ssize_t write_to_channel(const Channel& channel, int* savedErrno);
    void notify_write_complete_callback();
    void on_close(Channel& channel);
    void close_connection(Channel& channel); // 统一停心跳、关事件并向上交还连接。
    void notify_close_callback();
    void on_error(Channel& channel);
    void record_socket_error(const char* action, int errorCode); // 记录 errno 与诊断文本。
    void notify_error_callback();
    void notify_high_water_mark_callback();
    void tie_channel_to_owner();
    void start_app_heartbeat_timer(); // 重建周期心跳定时器。
    void stop_app_heartbeat_timer();
    void on_heartbeat_tick(); // 判超时并按需发送心跳探测包。
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
