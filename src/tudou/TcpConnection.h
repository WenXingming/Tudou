/**
 * @file TcpConnection.h
 * @brief 面向连接的 TCP 会话封装，负责收发缓冲、事件回调与状态管理。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
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
 * - read_callback(): 读取数据至 readBuffer，触发 message 回调。
 * - write_callback(): 将 writeBuffer 数据写入内核，必要时关闭写事件关注。
 * - close_callback(): 处理对端关闭并发布 close 回调，由上层完成资源回收。
 * - error_callback(): 记录错误并执行必要清理。
 *
 * 错误处理与边界：
 * - 考虑 EAGAIN/EWOULDBLOCK、短读/短写、对端半关闭等场景。
 * - 背压：send() 追加数据到 writeBuffer 并开启写事件，避免阻塞写。
 */

#pragma once
#include <memory>
#include <functional>
#include <string>

class EventLoop;
class Channel;
class Buffer;
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
    // 上层业务回调函数类型定义
    // 上层使用下层，所以参数是下层类型，因为一般通过 composition 来使用下层类，参数一般是指针或引用
    // 虽然理论上 MessageCallback 参数只使用 string，CloseCallback 参数只使用 fd，但是其实传入 TcpConnection 更方便上层获取更多信息。特别是使用 shared_ptr，避免回调过程中对象被析构
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

private:
    EventLoop* loop;
    std::unique_ptr<Channel> channel;
    std::unique_ptr<Buffer> readBuffer;
    std::unique_ptr<Buffer> writeBuffer;
    MessageCallback messageCallback{ nullptr }; // 业务层回调（类似于 ROS 发布话题）
    CloseCallback closeCallback{ nullptr }; // 应该是 TcpServer 回调（类似于 ROS 发布话题）

public:
    TcpConnection(EventLoop* _loop, int _sockfd);
    ~TcpConnection();

    void init_channel();

    int get_fd() const;
    void set_message_callback(MessageCallback _cb);
    void set_close_callback(CloseCallback _cb);

    // 公开接口，供上层业务层调用
    void send(const std::string& msg);
    std::string receive();

    // void shutdown(); // 暂时服务端不提供主动关闭连接接口

private:
    // 处理 channel 事件的上层回调函数
    void read_callback(Channel& channel);
    void write_callback(Channel& channel);
    void close_callback(Channel& channel);
    void error_callback(Channel& channel);

    // 触发上层回调
    void handle_message();
    void handle_close();
};
