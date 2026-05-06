// ============================================================================
// Acceptor.h
// 监听接入器，负责创建监听 socket、接收新连接，并把结果直接发布给上层。
// ============================================================================

#pragma once

#include <functional>
#include <memory>

#include "base/InetAddress.h"

class EventLoop;
class Channel;

// Acceptor 只负责“监听并发布新连接”，不参与连接生命周期管理。
class Acceptor {
public:
    using NewConnectCallback = std::function<void(int connFd, const InetAddress& peerAddr)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr);
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    ~Acceptor();

    /**
     * @brief 设置新连接回调。
     * @param cb accept 成功后触发的回调。
     */
    void set_connect_callback(NewConnectCallback cb);

    /**
     * @brief 获取监听 socket fd。
     * @return 当前监听 fd。
     */
    int get_listen_fd() const;

private:
    /**
     * @brief 创建监听 socket。
     * @return 处于 non-blocking 和 close-on-exec 模式的监听 fd。
     */
    int create_fd();

    /**
     * @brief 将监听地址绑定到 socket。
     * @param listenFd 监听 socket fd。
     */
    void bind_address(int listenFd);

    /**
     * @brief 启动监听状态。
     * @param listenFd 监听 socket fd。
     */
    void start_listen(int listenFd);

    /**
     * @brief 绑定监听 Channel 的事件回调。
     */
    void bind_channel_callbacks();

    /**
     * @brief 处理监听 socket 的错误事件。
     * @param channel 触发事件的监听 Channel。
     */
    void on_error(Channel& channel);

    /**
     * @brief 处理监听 socket 的关闭事件。
     * @param channel 触发事件的监听 Channel。
     */
    void on_close(Channel& channel);

    /**
     * @brief 处理监听 socket 的异常写事件。
     * @param channel 触发事件的监听 Channel。
     */
    void on_write(Channel& channel);

    /**
     * @brief 处理监听 socket 的可读事件，是接入流程的唯一编排入口。
     * @param channel 触发事件的监听 Channel。
     */
    void on_read(Channel& channel);

    /**
     * @brief 接收一个新连接。
     * @param clientAddr 输出参数，返回对端地址。
     * @return 成功时返回连接 fd，失败时返回 -1。
     */
    int accept_connection(sockaddr_in* clientAddr) const;

    /**
     * @brief 判断 accept 失败是否属于可恢复的瞬时错误。
     * @param errorCode 本次 accept 失败的 errno。
     * @return true 表示当前错误可以等待下一次可读事件后重试。
     */
    bool is_transient_accept_error(int errorCode) const;

    /**
     * @brief 将接收到的新连接发布给上层。
     * @param connFd 新连接 fd。
     * @param clientAddr 对端地址。
     */
    void publish_connection(int connFd, const sockaddr_in& clientAddr);

    /**
     * @brief 触发上层新连接回调。
     * @param connFd 新连接 fd。
     * @param peerAddr 对端地址。
     */
    void handle_connect_callback(int connFd, const InetAddress& peerAddr);

private:
    EventLoop* loop_; // 所属 EventLoop，定义 Acceptor 的线程边界。
    InetAddress listenAddr_; // 监听地址契约。
    int listenFd_; // 监听 socket fd，由监听 Channel 负责最终关闭。
    std::unique_ptr<Channel> channel_; // 监听 socket 对应的事件通道。
    NewConnectCallback newConnectCallback_; // accept 成功后向上发布连接的回调。
};
