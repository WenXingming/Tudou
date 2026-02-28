/**
 * @file TcpConnection.cpp
 * @brief 面向连接的 TCP 会话封装，负责收发缓冲、事件回调与状态管理。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "TcpConnection.h"

#include <assert.h>
#include <sys/socket.h>
#include <unistd.h>

#include "spdlog/spdlog.h"
#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"

TcpConnection::TcpConnection(EventLoop* loop, int connFd, const InetAddress& localAddr, const InetAddress& peerAddr) :
    loop_(loop),
    channel_(std::make_unique<Channel>(loop, connFd)),
    localAddr_(localAddr),
    peerAddr_(peerAddr),
    highWaterMark_(64 * 1024 * 1024), // 64 MB
    readBuffer_(std::make_unique<Buffer>()),
    writeBuffer_(std::make_unique<Buffer>()), // Don't forget! Or cause segfault!
    messageCallback_(nullptr),
    closeCallback_(nullptr),
    errorCallback_(nullptr),
    writeCompleteCallback_(nullptr),
    highWaterMarkCallback_(nullptr),
    lastErrorCode_(0),
    lastErrorMsg_("") {

    channel_->set_read_callback([this](Channel& ch) { on_read(ch); });
    channel_->set_write_callback([this](Channel& ch) { on_write(ch); });
    channel_->set_close_callback([this](Channel& ch) { on_close(ch); });
    channel_->set_error_callback([this](Channel& ch) { on_error(ch); });
    channel_->enable_reading();
}

TcpConnection::~TcpConnection() {
    spdlog::debug("TcpConnection::~TcpConnection() called. fd: {}", channel_->get_fd());
}

void TcpConnection::send(const std::string& msg) {
    // TODO: 考虑增加使用 run_in_loop 提供跨线程调用
    loop_->assert_in_loop_thread();

    size_t oldLen = writeBuffer_->readable_bytes();
    writeBuffer_->write_to_buffer(msg);
    size_t newLen = writeBuffer_->readable_bytes();

    // 检查是否超过高水位
    if (highWaterMarkCallback_ && oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        handle_high_water_mark_callback();
    }

    channel_->enable_writing();
}

std::string TcpConnection::receive() {
    loop_->assert_in_loop_thread();
    return readBuffer_->read_from_buffer(); // 不能用 run_in_loop，局部变量会在函数返回后失效
}

void TcpConnection::set_message_callback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpConnection::set_close_callback(CloseCallback cb) {
    closeCallback_ = std::move(cb);
}

void TcpConnection::set_error_callback(ErrorCallback cb) {
    errorCallback_ = std::move(cb);
}

void TcpConnection::set_write_complete_callback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpConnection::set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark) {
    highWaterMarkCallback_ = std::move(cb);
    highWaterMark_ = highWaterMark;
}

void TcpConnection::connection_establish() {
    auto ptr = shared_from_this();
    channel_->tie_to_object(ptr);
}

void TcpConnection::on_read(Channel& channel) {
    loop_->assert_in_loop_thread();

    // 从 fd 读数据到 readBuffer，然后触发上层回调处理数据
    int fd = channel.get_fd();
    int savedErrno = 0;
    ssize_t n = readBuffer_->read_from_fd(fd, &savedErrno);
    if (n > 0) {
        handle_message_callback();
        return;
    }
    // 对端关闭
    if (n == 0) {
        on_close(channel);
        return;
    }
    // 发生错误
    // 本轮数据读完，等下次 EPOLLIN 事件再读
    if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
        return;
    }
    lastErrorCode_ = savedErrno;
    lastErrorMsg_ = "read error: " + std::to_string(savedErrno);
    spdlog::error("TcpConnection::on_read(). read error: {}", savedErrno);
    on_error(channel);
}

void TcpConnection::on_write(Channel& channel) {
    loop_->assert_in_loop_thread();

    if (!channel.is_writing()) {
        spdlog::error("TcpConnection::on_write() but channel is not writing.");
        return;
    }

    int savedErrno = 0;
    int fd = channel.get_fd();
    ssize_t n = writeBuffer_->write_to_fd(fd, &savedErrno);

    if (n == -1) {
        // 本轮写完，等下次 EPOLLOUT 事件再写
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            return;
        }
        lastErrorCode_ = savedErrno;
        lastErrorMsg_ = "write error: " + std::to_string(savedErrno);
        spdlog::error("TcpConnection::on_write(). write error: {}", savedErrno);
        on_error(channel);
    }

    if (writeBuffer_->readable_bytes() == 0) {
        channel.disable_writing();
        handle_write_complete_callback();
    }
}

void TcpConnection::on_close(Channel& channel) {
    loop_->assert_in_loop_thread();

    channel.disable_all();
    handle_close_callback();
}

void TcpConnection::on_error(Channel& channel) {
    loop_->assert_in_loop_thread();

    handle_error_callback();
    on_close(channel);
}

void TcpConnection::handle_message_callback() {
    assert(messageCallback_ != nullptr);
    messageCallback_(shared_from_this());
}

void TcpConnection::handle_close_callback() {
    assert(closeCallback_ != nullptr);

    std::shared_ptr<TcpConnection> guardThis{ shared_from_this() }; // 防止回调过程中对象被析构，延长生命周期
    closeCallback_(guardThis);
}

void TcpConnection::handle_error_callback() {
    if (!errorCallback_) {
        spdlog::warn("TcpConnection::handle_error_callback(). errorCallback is nullptr, fd: {}", get_fd());
        return;
    }
    errorCallback_(shared_from_this());
}

void TcpConnection::handle_write_complete_callback() {
    if (!writeCompleteCallback_) {
        spdlog::warn("TcpConnection::handle_write_complete_callback(). writeCompleteCallback is nullptr, fd: {}", get_fd());
        return;
    }
    writeCompleteCallback_(shared_from_this());
}

void TcpConnection::handle_high_water_mark_callback() {
    if (!highWaterMarkCallback_) {
        spdlog::warn("TcpConnection::handle_high_water_mark_callback(). highWaterMarkCallback is nullptr, fd: {}", get_fd());
        return;
    }
    highWaterMarkCallback_(shared_from_this());
}
