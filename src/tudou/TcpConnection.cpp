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
    channel.reset(new Channel(_loop, _connFd));
    channel->set_read_callback(std::bind(&TcpConnection::read_callback, this));
    channel->set_write_callback([this]() { this->write_callback(); });
    channel->set_close_callback([this]() { this->close_callback(); });
    channel->set_error_callback([this]() { this->error_callback(); });
    channel->enable_reading();
    channel->update_to_register(); // 注册到 poller，和 TcpConnection 创建同步

    // 初始化缓冲区, unique_ptr 自动管理内存。Don't forget! Or cause segfault!
    readBuffer.reset(new Buffer());
    writeBuffer.reset(new Buffer());
}

TcpConnection::~TcpConnection() {

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
void TcpConnection::read_callback() {
    int savedErrno = 0;
    ssize_t n = readBuffer->read_from_fd(this->channel->get_fd(), &savedErrno);
    if (n > 0) {
        // 从 fd 读取到了数据到 buffer，触发上层回调逻辑处理数据
        this->handle_message();
    }
    else if (n == 0) { // 对端关闭
        this->close_callback();
    }
    else {
        spdlog::error("TcpConnection::read_callback(). read error: {}", savedErrno);
        this->close_callback();
    }
}

// 从 writeBuffer 写数据到 fd，写完了就取消对写事件的关注
void TcpConnection::write_callback() {
    int savedErrno = 0;
    ssize_t n = writeBuffer->write_to_fd(this->channel->get_fd(), &savedErrno);
    if (n > 0) {
        if (writeBuffer->readable_bytes() == 0) { // writeBuffer 里的数据写完了
            channel->disable_writing();
        }
    }
    else {
        spdlog::error("TcpConnection::write_callback(). write error: {}", savedErrno);
        this->close_callback(); // 是否需要关闭连接？
    }
}

void TcpConnection::close_callback() {
    channel->disable_all();
    // channel->remove_in_register();
    // fd 生命期由 TcpConnection 管理（绑定）。因此这里不关闭 fd，析构函数中关闭

    // 触发上层 TcpServer 回调，进行资源回收（删除 TcpConnection shared_ptr 对象）
    this->handle_close();
}

void TcpConnection::error_callback() {
    spdlog::error("TcpConnection::error_callback() called.");
    this->close_callback();
}

void TcpConnection::handle_message() {
    assert(messageCallback != nullptr);
    messageCallback(shared_from_this());
}

void TcpConnection::handle_close() {
    assert(closeCallback != nullptr);
    closeCallback(shared_from_this());
}
