// ============================================================================
// TcpConnection.h
// TcpConnection 负责单个连接的收发、关闭和心跳流程，通过 Socket 持有连接 fd。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// TcpConnection.h
// └── TcpConnection
//     ├── TcpConnection(loop, connSocket, localAddr, peerAddr)  # [公有] 构造：接管 Socket，绑定 Channel 回调并开启读
//     │   ├── on_read(channel)                       # [私有] 读事件主干：读数据、判 EOF、判错误
//     │   │   ├── read_from_channel(channel, &err)   # [私有] 从 fd 搬运数据到 readBuffer_
//     │   │   ├── refresh_last_read_time()           # [私有] 有入站数据就刷新活跃时间
//     │   │   ├── notify_message_callback()          # [私有] 把消息事件抛给上层
//     │   │   ├── record_socket_error("read", err)   # [私有] 记录读失败错误信息
//     │   │   ├── notify_error_callback()            # [私有] 通知上层错误
//     │   │   └── close_connection(channel)          # [私有] EOF 或致命错误统一关闭
//     │   │       ├── stop_app_heartbeat_timer()     # [私有] 取消心跳定时器
//     │   │       └── notify_close_callback()        # [私有] 触发服务器侧连接移除
//     │   ├── on_write(channel)                      # [私有] 可写事件主干：刷发送缓冲
//     │   │   ├── write_to_channel(channel, &err)    # [私有] 把 writeBuffer_ 写入 fd
//     │   │   ├── notify_write_complete_callback()   # [私有] 写缓冲清空时通知上层
//     │   │   ├── record_socket_error("write", err)  # [私有] 记录写失败错误信息
//     │   │   ├── notify_error_callback()            # [私有] 通知上层错误
//     │   │   └── close_connection(channel)          # [私有] 致命写错误统一收口
//     │   │       ├── stop_app_heartbeat_timer()     # [私有] 取消心跳定时器
//     │   │       └── notify_close_callback()        # [私有] 触发服务器侧连接移除
//     │   ├── on_close(channel)                      # [私有] close 事件入口
//     │   │   └── close_connection(channel)          # [私有] 幂等关闭（同上）
//     │   └── on_error(channel)                      # [私有] error 事件入口
//     │       ├── notify_error_callback()            # [私有] 向上转发错误
//     │       └── close_connection(channel)          # [私有] 与 read/write 错误共用收尾
//     ├── ~TcpConnection()                       # [公有] 析构：connSocket_ 自动关闭 fd
//     ├── send(msg)                              # [公有] 统一发送入口，先入写缓冲再注册写事件
//     │   └── notify_high_water_mark_callback()  # [私有] 越过高水位阈值时上报背压
//     ├── receive()                              # [公有] 拉取并清空当前读缓冲中的应用层数据
//     ├── connection_establish()                 # [公有] 建立 shared_from_this 保活并补启心跳
//     │   ├── tie_channel_to_owner()             # [私有] 将自身绑到 Channel tie 机制
//     │   └── start_app_heartbeat_timer()        # [私有] 若已启用心跳则创建周期任务
//     │       ├── stop_app_heartbeat_timer()     # [私有] 先取消旧定时器
//     │       └── on_heartbeat_tick()            # [私有] run_every 到期后执行心跳 tick
//     │           ├── is_heartbeat_timeout(now) const  # [私有] 判断连接是否空闲超时
//     │           ├── close_connection(*channel_)      # [私有] 超时则主动关闭
//     │           └── send(heartbeatPingMessage_)      # [公有] 未超时则发探测包
//     ├── enable_app_heartbeat(interval, timeout, ping)  # [公有] 打开心跳并刷新活跃时间
//     │   ├── refresh_last_read_time()           # [私有] 重置最近收包时间
//     │   └── start_app_heartbeat_timer()        # [私有] 重建周期定时器
//     ├── disable_app_heartbeat()                # [公有] 关闭心跳并取消定时器
//     │   └── stop_app_heartbeat_timer()         # [私有] cancel 当前 TimerId
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
#include "Socket.h"
#include "Timer.h"

class EventLoop;

// TcpConnection 是面向连接的会话门面，通过 Socket 持有连接 fd 所有权。
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using ErrorCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using WriteCompleteCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using HighWaterMarkCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    TcpConnection(EventLoop* loop, Socket connSocket, const InetAddress& localAddr, const InetAddress& peerAddr);
    ~TcpConnection();

    void send(const std::string& msg);
    std::string receive();

    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);
    void set_error_callback(ErrorCallback cb);
    void set_write_complete_callback(WriteCompleteCallback cb);
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark);

    void connection_establish();
    void enable_app_heartbeat(double intervalSeconds, double timeoutSeconds, const std::string& pingMessage = "PING\r\n");
    void disable_app_heartbeat();

    bool is_app_heartbeat_enabled() const { return heartbeatEnabled_; }
    EventLoop* get_loop() const { return loop_; }
    int get_fd() const { return connSocket_.fd(); }
    const InetAddress& get_local_addr() const { return localAddr_; }
    const InetAddress& get_peer_addr() const { return peerAddr_; }
    int get_last_error() const { return lastErrorCode_; }
    const std::string& get_last_error_msg() const { return lastErrorMsg_; }
    size_t get_write_buffer_size() const { return writeBuffer_->readable_bytes(); }
    size_t get_high_water_mark() const { return highWaterMark_; }

private:
    void on_read(Channel& channel);
    ssize_t read_from_channel(const Channel& channel, int* savedErrno);
    void refresh_last_read_time();
    void notify_message_callback();
    void on_write(Channel& channel);
    ssize_t write_to_channel(const Channel& channel, int* savedErrno);
    void notify_write_complete_callback();
    void on_close(Channel& channel);
    void close_connection(Channel& channel);
    void notify_close_callback();
    void on_error(Channel& channel);
    void record_socket_error(const char* action, int errorCode);
    void notify_error_callback();
    void notify_high_water_mark_callback();
    void tie_channel_to_owner();
    void start_app_heartbeat_timer();
    void stop_app_heartbeat_timer();
    void on_heartbeat_tick();
    bool is_heartbeat_timeout(std::chrono::steady_clock::time_point now) const;

private:
    EventLoop* loop_;
    Socket connSocket_;                          // 连接 socket 的 RAII 句柄，析构时自动关闭 fd（必须在 channel_ 之前声明）
    std::unique_ptr<Channel> channel_;
    InetAddress localAddr_;
    InetAddress peerAddr_;
    size_t highWaterMark_;
    std::unique_ptr<Buffer> readBuffer_;
    std::unique_ptr<Buffer> writeBuffer_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    ErrorCallback errorCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    int lastErrorCode_;
    std::string lastErrorMsg_;
    bool isClosed_;
    bool heartbeatEnabled_;
    double heartbeatIntervalSeconds_;
    double heartbeatTimeoutSeconds_;
    std::string heartbeatPingMessage_;
    TimerId heartbeatTimerId_;
    std::chrono::steady_clock::time_point lastReadTime_;
};
