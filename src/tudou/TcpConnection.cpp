/**
 * @file TcpConnection.h
 * @brief 面向连接的 TCP 会话封装，负责收发缓冲、事件回调与状态管理。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
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

TcpConnection::TcpConnection(EventLoop* _loop, int _connFd)
    : loop(_loop) {

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
    channel->update_in_register(); // 注册到 poller，和 TcpConnection 创建同步

    // 初始化缓冲区, unique_ptr 自动管理内存。Don't forget! Or cause segfault!
    readBuffer.reset(new Buffer());
    writeBuffer.reset(new Buffer());
}

TcpConnection::~TcpConnection() {

}

void TcpConnection::init_channel() {
    channel->tie_to_object(shared_from_this());
}

int TcpConnection::get_fd() const {
    return this->channel->get_fd();
}

void TcpConnection::set_message_callback(MessageCallback _cb) {
    this->messageCallback = std::move(_cb);
}

void TcpConnection::set_close_callback(CloseCallback _cb) {
    this->closeCallback = std::move(_cb);
}

void TcpConnection::send(const std::string& msg) {
    writeBuffer->write_to_buffer(msg);
    channel->enable_writing();
}

std::string TcpConnection::receive() {
    std::string msg(readBuffer->read_from_buffer());
    return std::move(msg);
}

/* void TcpConnection::shutdown() {
    ::shutdown(this->connectFd, SHUT_WR);
} */

// 从 fd 读数据到 readBuffer，然后触发上层回调处理数据
void TcpConnection::on_read(Channel& channel) {
    int fd = channel.get_fd();
    int savedErrno = 0;
    ssize_t n = readBuffer->read_from_fd(fd, &savedErrno);
    if (n > 0) {
        // 从 fd 读取到了数据到 buffer，触发上层回调逻辑处理数据
        this->handle_message_callback();
    }
    else if (n == 0) { // 对端关闭
        this->on_close(channel);
    }
    else {
        spdlog::error("TcpConnection::on_read(). read error: {}", savedErrno);
        this->on_close(channel);
    }
}

// 从 writeBuffer 写数据到 fd，写完了就取消对写事件的关注
void TcpConnection::on_write(Channel& channel) {
    int savedErrno = 0;
    ssize_t n = writeBuffer->write_to_fd(channel.get_fd(), &savedErrno);
    if (n > 0) {
        if (writeBuffer->readable_bytes() == 0) { // writeBuffer 里的数据写完了
            channel.disable_writing();
        }
    }
    else {
        spdlog::error("TcpConnection::on_write(). write error: {}", savedErrno);
        this->on_close(channel); // 是否需要关闭连接？
    }
}

void TcpConnection::on_close(Channel& channel) {
    spdlog::info("TcpConnection::on_close() called. fd: {}", channel.get_fd());
    channel.disable_all();

    // 触发上层 TcpServer 回调，进行资源回收（删除 TcpConnection shared_ptr 对象）
    this->handle_close_callback();
}

void TcpConnection::on_error(Channel& channel) {
    spdlog::error("TcpConnection::on_error() called.");
    this->on_close(channel);
}

void TcpConnection::handle_message_callback() {
    assert(messageCallback != nullptr);
    messageCallback(shared_from_this());
}

void TcpConnection::handle_close_callback() {
    assert(closeCallback != nullptr);
    closeCallback(shared_from_this());
}
