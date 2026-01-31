/**
 * @file TcpConnection.h
 * @brief 面向连接的 TCP 会话封装，负责收发缓冲、事件回调与状态管理。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 * @details
 *
 * 职责：
 * - 封装已建立连接的 fd（connectFd）及其 Channel（持有），处理读/写/关闭/错误事件。
 * - 维护读写 Buffer，向上提供 send()/receive()，并通过回调对接业务层与 TcpServer。
 * - 对外暴露 set_message_callback()/set_close_callback() 以设置回调，实现与上层解耦。
 *
 * 线程模型与约定：
 * - 与所属 EventLoop 线程绑定；除可封装为投递的接口外，方法应在该线程调用。
 * - 非线程安全；如需跨线程调用，建议配合 runInLoop/queueInLoop 与唤醒机制（若后续引入）。
 *
 * 生命周期与所有权：
 * - 使用 enable_shared_from_this，自保活以保证回调执行期对象有效。
 * - 持有 Channel/Buffer 的唯一所有权；不拥有上层对象，仅保存回调函数。
 *
 * I/O 与回调：
 * - on_read(): 读取数据至 readBuffer，触发 message 回调。
 * - on_write(): 将 writeBuffer 数据写入内核，必要时关闭写事件关注。
 * - on_close(): 处理对端关闭并发布 close 回调，由上层完成资源回收。
 * - on_error(): 记录错误并执行必要清理。
 *
 * 错误处理与边界：
 * - 考虑 EAGAIN/EWOULDBLOCK、短读/短写、对端半关闭等场景。
 * - 背压：send() 追加数据到 writeBuffer 并开启写事件，避免阻塞写。
 */

#pragma once

#include <memory>
#include <functional>
#include <string>

#include "base/InetAddress.h"

class EventLoop;
class Channel;
class Buffer;
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
    // 参数设计：上层使用下层，所以参数是下层类型，因为一般通过 composition 来使用下层类，参数一般是指针或引用
    // 通常：using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    // 解释：
    // 虽然理论上 MessageCallback 参数只使用 string，CloseCallback 参数只使用 fd，但是其实传入 TcpConnection 更方便上层获取更多信息。特别是使用 shared_ptr，避免回调过程中对象被析构
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using ErrorCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;                   // 错误回调，通过 get_last_error() 获取错误码
    using WriteCompleteCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;           // 写完成回调，当 writeBuffer 数据全部写入内核后触发
    using HighWaterMarkCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;           // 高水位回调，当 writeBuffer 积压超过高水位时触发，通过 get_write_buffer_size() 获取当前积压大小

    // TODO: 添加状态枚举（连接状态管理），如 kConnecting, kConnected, kDisconnecting, kDisconnected 和状态检查

private:
    EventLoop* loop;
    std::unique_ptr<Channel> channel;
    InetAddress localAddr;  // 本地地址
    InetAddress peerAddr;   // 对端地址
    size_t highWaterMark;   // 高水位标记，单位字节
    std::unique_ptr<Buffer> readBuffer;
    std::unique_ptr<Buffer> writeBuffer;

    // 回调函数。数据流向上层，触发上层逻辑处理（通过 tcpConn->receive()、 tcpConn->send() 接口控制数据流）
    MessageCallback messageCallback;                // 业务层回调：接收到数据时触发
    CloseCallback closeCallback;                    // TcpServer 回调：连接关闭时触发
    ErrorCallback errorCallback;                    // 错误回调：发生错误时触发
    WriteCompleteCallback writeCompleteCallback;    // 写完成回调：数据全部写入内核时触发
    HighWaterMarkCallback highWaterMarkCallback;    // 高水位回调：writeBuffer 积压超过高水位时触发

    // 错误信息（遵循高内聚原则，保存在 TcpConnection 内部）
    int lastErrorCode;       // 最近一次的错误码（errno）
    std::string lastErrorMsg; // 最近一次的错误描述

public:
    TcpConnection(EventLoop* _loop, int _connFd, const InetAddress& _localAddr, const InetAddress& _peerAddr);
    ~TcpConnection();

    void connection_establish();

    EventLoop* get_loop() const { return loop; }
    int get_fd() const;
    const InetAddress& get_local_addr() const { return localAddr; }
    const InetAddress& get_peer_addr() const { return peerAddr; }

    // 设置回调函数
    void set_message_callback(MessageCallback _cb);
    void set_close_callback(CloseCallback _cb);
    void set_error_callback(ErrorCallback _cb);
    void set_write_complete_callback(WriteCompleteCallback _cb);
    void set_high_water_mark_callback(HighWaterMarkCallback _cb, size_t highWaterMark);

    // 获取错误信息（高内聚：错误信息封装在 TcpConnection 内部）
    int get_last_error() const { return lastErrorCode; }
    const std::string& get_last_error_msg() const { return lastErrorMsg; }

    // 获取 buffer 相关信息（高内聚：buffer 状态封装在 TcpConnection 内部）
    size_t get_write_buffer_size() const;  // 获取 writeBuffer 当前积压大小
    size_t get_high_water_mark() const { return highWaterMark; }  // 获取高水位设置

    // 公开接口，供上层业务层调用
    void send(const std::string& msg);
    std::string receive();  // 从 readBuffer 读取所有数据
    /// TODO: 未来可以添加更多灵活的读取接口，如 receive(size_t len), readable_bytes(), peek() 等

    // void shutdown(); // 暂时服务端不提供主动关闭连接接口

private:
    // 处理 channel 事件的上层回调函数
    void on_read(Channel& channel);
    void on_write(Channel& channel);
    void on_close(Channel& channel);
    void on_error(Channel& channel);

    // 触发上层回调
    void handle_message_callback();
    void handle_close_callback();
    void handle_error_callback();
    void handle_write_complete_callback();
    void handle_high_water_mark_callback();
};
