// ============================================================================
// TcpConnection.h
// TcpConnection 负责单个连接的收发和关闭流程，通过 Socket 持有连接 fd。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// TcpConnection.h
// └── TcpConnection
//     ├── create(loop, connSocket, localAddr, peerAddr)      # [公有] 构造并完成激活，隐藏 tie 细节
//     │   ├── TcpConnection(loop, connSocket, localAddr, peerAddr)  # [私有] 接管 Socket，绑定 Channel 回调
//     │   └── activate()                               # [私有] 在 shared_ptr 生效后建立 tie 并开启读
//     │   ├── on_read(channel)                       # [私有] 读事件主干：读数据、判 EOF、判错误
//     │   │   ├── handle_message_callback()          # [私有] 把消息事件抛给上层
//     │   │   ├── handle_error_callback()            # [私有] 通知上层错误
//     │   │   └── close_connection(channel)          # [私有] EOF 或致命错误统一关闭
//     │   │       └── handle_close_callback()        # [私有] 触发服务器侧连接移除
//     │   ├── on_write(channel)                      # [私有] 可写事件主干：刷发送缓冲
//     │   │   ├── handle_write_complete_callback()   # [私有] 写缓冲清空时通知上层
//     │   │   ├── handle_error_callback()            # [私有] 通知上层错误
//     │   │   └── close_connection(channel)          # [私有] 致命写错误统一收口
//     │   │       └── handle_close_callback()        # [私有] 触发服务器侧连接移除
//     │   ├── on_close(channel)                      # [私有] close 事件入口
//     │   │   └── close_connection(channel)          # [私有] 幂等关闭（同上）
//     │   └── on_error(channel)                      # [私有] error 事件入口
//     │       ├── handle_error_callback()            # [私有] 向上转发错误
//     │       └── close_connection(channel)          # [私有] 与 read/write 错误共用收尾
//     ├── ~TcpConnection()                       # [公有] 析构：connSocket_ 自动关闭 fd
//     ├── send(msg)                              # [公有] 线程安全发送入口，必要时投递回所属 EventLoop
//     │   └── send_in_loop(msg)                  # [私有] 先入写缓冲再注册写事件
//     │       └── handle_high_water_mark_callback()  # [私有] 越过高水位阈值时上报背压
//     ├── receive()                              # [公有] 拉取并清空当前读缓冲中的应用层数据
//     ├── force_close()                          # [公有] 主动关闭连接，供上层策略对象调用
//     │   └── force_close_in_loop()              # [私有] 与被动关闭共用收尾路径
//     ├── set_message_callback(cb)               # [公有] 注册消息回调
//     ├── set_close_callback(cb)                 # [公有] 注册关闭回调
//     ├── set_error_callback(cb)                 # [公有] 注册错误回调
//     ├── set_write_complete_callback(cb)        # [公有] 注册写完成回调
//     ├── set_high_water_mark_callback(cb, mark) # [公有] 注册高水位回调并更新阈值
//     ├── get_loop() const                       # [公有] 返回所属 EventLoop
//     ├── get_fd() const                         # [公有] 返回连接 fd
//     ├── get_local_addr() const                 # [公有] 返回本地地址快照
//     ├── get_peer_addr() const                  # [公有] 返回对端地址快照
//     ├── get_write_buffer_size() const          # [公有] 返回当前发送积压字节数
//     └── get_high_water_mark() const            # [公有] 返回高水位阈值
// ============================================================================

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "base/InetAddress.h"
#include "Buffer.h"
#include "Channel.h"
#include "Socket.h"

class EventLoop;

// TcpConnection 是面向连接的会话门面，通过 Socket 持有连接 fd 所有权。
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using ErrorCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using WriteCompleteCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using HighWaterMarkCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    static std::shared_ptr<TcpConnection> create(EventLoop* loop,
        Socket connSocket,
        const InetAddress& localAddr,
        const InetAddress& peerAddr);

    ~TcpConnection();

    void send(const std::string& msg);
    std::string receive();

    void set_tcp_no_delay(bool on);
    void set_keep_alive(bool on);

    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);
    void set_error_callback(ErrorCallback cb);
    void set_write_complete_callback(WriteCompleteCallback cb);
    void set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark);

    void force_close();

    EventLoop* get_loop() const { return loop_; }
    int get_fd() const { return connSocket_.fd(); }
    const InetAddress& get_local_addr() const { return localAddr_; }
    const InetAddress& get_peer_addr() const { return peerAddr_; }
    size_t get_write_buffer_size() const { return writeBuffer_->readable_bytes(); }
    size_t get_high_water_mark() const { return highWaterMark_; }

private:
    TcpConnection(EventLoop* loop, Socket connSocket, const InetAddress& localAddr, const InetAddress& peerAddr);
    void activate();

    void send_in_loop(const std::string& msg);
    void on_read(Channel& channel);
    void handle_message_callback();
    void on_write(Channel& channel);
    void handle_write_complete_callback();
    void force_close_in_loop();
    void on_close(Channel& channel);
    void close_connection(Channel& channel);
    void handle_close_callback();
    void on_error(Channel& channel);
    void handle_error_callback();
    void handle_high_water_mark_callback();
private:
    EventLoop* loop_; // 所属 EventLoop，所有回调均在此线程执行。

    Socket connSocket_;                          // 连接 socket 的 RAII 句柄，析构时自动关闭 fd（必须在 channel_ 之前声明）
    std::unique_ptr<Channel> channel_; // 连接 fd 对应的 Channel，负责 epoll 事件回调。

    InetAddress localAddr_; // 本地地址快照。
    InetAddress peerAddr_;  // 对端地址快照。

    std::unique_ptr<Buffer> readBuffer_;  // 应用层读缓冲。
    std::unique_ptr<Buffer> writeBuffer_; // 应用层写缓冲。

    size_t highWaterMark_; // 发送缓冲高水位阈值（字节）。

    MessageCallback messageCallback_;               // 消息到达时触发（必选）。
    CloseCallback closeCallback_;                   // 连接关闭时触发（必选）。
    ErrorCallback errorCallback_;                   // 读写错误时触发（可选）。
    WriteCompleteCallback writeCompleteCallback_;   // 写缓冲清空时触发（可选）。
    HighWaterMarkCallback highWaterMarkCallback_;   // 写缓冲越过高水位时触发（可选）。

    bool isClosed_; // 是否已关闭，保证 close_connection 幂等。
};
