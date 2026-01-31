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

TcpConnection::TcpConnection(EventLoop* _loop, int _connFd, const InetAddress& _localAddr, const InetAddress& _peerAddr) :
    loop(_loop),
    channel(nullptr),
    localAddr(_localAddr),
    peerAddr(_peerAddr),
    highWaterMark(64 * 1024 * 1024), // 64 MB
    readBuffer(new Buffer()),
    writeBuffer(new Buffer()), // Don't forget! Or cause segfault!
    messageCallback(nullptr),
    closeCallback(nullptr),
    errorCallback(nullptr),
    writeCompleteCallback(nullptr),
    highWaterMarkCallback(nullptr),
    lastErrorCode(0),
    lastErrorMsg("") {

    // 初始化 channel. 创建 channel 后需要设置 intesting event 和 订阅（发生事件后的回调函数）
    channel.reset(new Channel(_loop, _connFd)); // 传入 shared_from_this 作为 tie，防止 handle_events_with_guard 过程中被销毁
    channel->set_read_callback(
        std::bind(&TcpConnection::on_read, this, std::placeholders::_1)
    );
    channel->set_write_callback([this](Channel& channel) {
        this->on_write(channel); }
    );
    channel->set_close_callback([this](Channel& channel) {
        this->on_close(channel); }
    );
    channel->set_error_callback([this](Channel& channel) {
        this->on_error(channel); }
    );
    channel->enable_reading();

}

TcpConnection::~TcpConnection() {
    spdlog::debug("TcpConnection::~TcpConnection() called. fd: {}", channel->get_fd());
}

void TcpConnection::connection_establish() {
    channel->tie_to_object(shared_from_this());
}

int TcpConnection::get_fd() const {
    return channel->get_fd();
}

void TcpConnection::set_message_callback(MessageCallback _cb) {
    messageCallback = std::move(_cb);
}

void TcpConnection::set_close_callback(CloseCallback _cb) {
    closeCallback = std::move(_cb);
}

void TcpConnection::set_error_callback(ErrorCallback _cb) {
    errorCallback = std::move(_cb);
}

void TcpConnection::set_write_complete_callback(WriteCompleteCallback _cb) {
    writeCompleteCallback = std::move(_cb);
}

void TcpConnection::set_high_water_mark_callback(HighWaterMarkCallback _cb, size_t _highWaterMark) {
    highWaterMarkCallback = std::move(_cb);
    highWaterMark = _highWaterMark;
}

size_t TcpConnection::get_write_buffer_size() const {
    loop->assert_in_loop_thread();
    return writeBuffer->readable_bytes();
}

void TcpConnection::send(const std::string& msg) {
    // 按理说会在 loop 线程调用 send，但某些应用场景可能会记录 TcpConnection 对象到其他线程使用
    // TODO: 考虑增加使用 run_in_loop 提供跨线程调用
    loop->assert_in_loop_thread();

    size_t oldLen = writeBuffer->readable_bytes();
    writeBuffer->write_to_buffer(msg);
    size_t newLen = writeBuffer->readable_bytes();

    // 检查是否超过高水位
    if (highWaterMarkCallback && oldLen < highWaterMark && newLen >= highWaterMark) {
        handle_high_water_mark_callback();
    }

    channel->enable_writing();
}

std::string TcpConnection::receive() {
    // 在对应的 IO 线程中执行读取操作
    loop->assert_in_loop_thread();
    std::string msg = readBuffer->read_from_buffer(); // Don't use run_in_loop here, stack variable msg will be invalid after function return
    return std::move(msg);
}

/* void TcpConnection::shutdown() {
    ::shutdown(this->connectFd, SHUT_WR);
} */

// 从 fd 读数据到 readBuffer，然后触发上层回调处理数据
void TcpConnection::on_read(Channel& channel) {
    loop->assert_in_loop_thread();

    int fd = channel.get_fd();
    int savedErrno = 0;
    ssize_t n = readBuffer->read_from_fd(fd, &savedErrno);
    if (n > 0) {
        // 从 fd 读取到了数据到 buffer，触发上层回调逻辑处理数据
        this->handle_message_callback();
    }
    else if (n == 0) { // 对端关闭
        on_close(channel);
    }
    else {
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            return; // 本轮数据读完，等下次 EPOLLIN 事件再读
        }
        else {
            lastErrorCode = savedErrno;
            lastErrorMsg = "read error: " + std::to_string(savedErrno);
            spdlog::error("TcpConnection::on_read(). read error: {}", savedErrno);
            on_error(channel);
        }
    }
}

// 从 writeBuffer 写数据到 fd，写完了就取消对写事件的关注
void TcpConnection::on_write(Channel& channel) {
    loop->assert_in_loop_thread();
    if (!channel.is_writing()) {
        spdlog::warn("TcpConnection::on_write() but channel is not writing.");
        return; // 避免重复触发
    }

    int savedErrno = 0;
    int fd = channel.get_fd();
    ssize_t n = writeBuffer->write_to_fd(fd, &savedErrno);
    if (n > 0) {
        if (writeBuffer->readable_bytes() == 0) { // writeBuffer 里的数据写完了
            channel.disable_writing();
            // 触发写完成回调
            if (writeCompleteCallback) {
                handle_write_complete_callback();
            }
        }
    }
    else {
        lastErrorCode = savedErrno;
        lastErrorMsg = "write error: " + std::to_string(savedErrno);
        spdlog::error("TcpConnection::on_write(). write error: {}", savedErrno);
        on_error(channel); // 发生错误就关闭连接
    }
}

void TcpConnection::on_close(Channel& channel) {
    loop->assert_in_loop_thread();
    spdlog::debug("TcpConnection::on_close() called. fd: {}", channel.get_fd());

    channel.disable_all();
    this->handle_close_callback(); // 触发上层 TcpServer 回调，进行资源回收（TcpServer 删除 TcpConnection shared_ptr 对象）
}

void TcpConnection::on_error(Channel& channel) {
    loop->assert_in_loop_thread();
    spdlog::error("TcpConnection::on_error() called. fd: {}, error: {}", channel.get_fd(), lastErrorMsg);

    // 触发错误回调（如果设置了）
    if (errorCallback) {
        handle_error_callback();
    }

    // 错误后关闭连接
    on_close(channel);
}

void TcpConnection::handle_message_callback() {
    assert(messageCallback != nullptr);
    messageCallback(shared_from_this());
}

void TcpConnection::handle_close_callback() {
    assert(closeCallback != nullptr);

    std::shared_ptr<TcpConnection> guardThis{ shared_from_this() }; // 防止回调过程中对象被析构，延长生命周期
    closeCallback(shared_from_this());
}

void TcpConnection::handle_error_callback() {
    if (errorCallback) {
        errorCallback(shared_from_this());
    }
}

void TcpConnection::handle_write_complete_callback() {
    if (writeCompleteCallback) {
        writeCompleteCallback(shared_from_this());
    }
}

void TcpConnection::handle_high_water_mark_callback() {
    if (highWaterMarkCallback) {
        highWaterMarkCallback(shared_from_this());
    }
}
