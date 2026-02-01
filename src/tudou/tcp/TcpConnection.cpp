/**
 * @file TcpConnection.cpp
 * @brief 面向连接的 TCP 会话封装，负责收发缓冲、事件回调与状态管理。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "TcpConnection.h"

#include <assert.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

#include "spdlog/spdlog.h"
#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"

TcpConnection::TcpConnection(EventLoop* loop, int connFd, const InetAddress& localAddr, const InetAddress& peerAddr) :
    loop_(loop),
    channel_(nullptr),
    localAddr_(localAddr),
    peerAddr_(peerAddr),
    highWaterMark_(64 * 1024 * 1024), // 64 MB
    readBuffer_(new Buffer()),
    writeBuffer_(new Buffer()), // Don't forget! Or cause segfault!
    messageCallback_(nullptr),
    closeCallback_(nullptr),
    errorCallback_(nullptr),
    writeCompleteCallback_(nullptr),
    highWaterMarkCallback_(nullptr),
    lastErrorCode_(0),
    lastErrorMsg_("") {

    // 初始化 channel: callback、interesting events
    channel_.reset(new Channel(loop_, connFd));
    channel_->set_read_callback(
        std::bind(&TcpConnection::on_read, this, std::placeholders::_1)
    );
    channel_->set_write_callback([this](Channel& channel) {
        this->on_write(channel); }
    );
    channel_->set_close_callback([this](Channel& channel) {
        this->on_close(channel); }
    );
    channel_->set_error_callback([this](Channel& channel) {
        this->on_error(channel); }
    );
    channel_->enable_reading();
}

TcpConnection::~TcpConnection() {
    spdlog::debug("TcpConnection::~TcpConnection() called. fd: {}", channel_->get_fd());
}

void TcpConnection::send(const std::string& msg) {
    // 按理说会在 loop 线程调用 send，但某些应用场景可能会记录 TcpConnection 对象到其他线程使用
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

    std::string msg = readBuffer_->read_from_buffer(); // Don't use run_in_loop here, stack variable msg will be invalid after function return
    return std::move(msg);
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

/* void TcpConnection::shutdown() {
    ::shutdown(this->connectFd, SHUT_WR);
} */

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
        this->handle_message_callback();
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
    else {
        lastErrorCode_ = savedErrno;
        lastErrorMsg_ = "read error: " + std::to_string(savedErrno);
        spdlog::error("TcpConnection::on_read(). read error: {}", savedErrno);
        on_error(channel);
    }
}

void TcpConnection::on_write(Channel& channel) {
    loop_->assert_in_loop_thread();

    // 从 writeBuffer 写数据到 fd，写完了就取消对写事件的关注
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
        // 其他错误
        lastErrorCode_ = savedErrno;
        lastErrorMsg_ = "write error: " + std::to_string(savedErrno);
        spdlog::error("TcpConnection::on_write(). write error: {}", savedErrno);
        on_error(channel);
    }

    // writeBuffer 里的数据写完了
    if (writeBuffer_->readable_bytes() == 0) {
        channel.disable_writing();
        handle_write_complete_callback();
    }
    return;

}

void TcpConnection::on_close(Channel& channel) {
    loop_->assert_in_loop_thread();

    channel.disable_all();
    this->handle_close_callback(); // 触发上层 TcpServer 回调，进行资源回收（TcpServer 删除 TcpConnection shared_ptr 对象）
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
    closeCallback_(shared_from_this());
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
