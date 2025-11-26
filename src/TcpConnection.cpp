/**
 * @file TcpConnection.h
 * @brief 面向连接的 TCP 会话封装，负责收发缓冲、事件回调与状态管理。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include <iostream>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include "TcpConnection.h"
#include "EventLoop.h"
#include "Channel.h"
#include "Buffer.h"
#include "../base/Timestamp.h"
#include "../base/Log.h"

TcpConnection::TcpConnection(EventLoop* _loop, int _connFd)
    : loop(_loop)
    , connectFd(_connFd)
    , readBuffer(new Buffer())
    , writeBuffer(new Buffer())
    , messageCallback(nullptr)
    , closeCallback(nullptr) {

    // 初始化 channel. 也可以放在初始化列表里，但注意初始化顺序（如果依赖其他成员变量的话）
    // 创建 channel 后需要设置 intesting event 和 订阅（发生事件后的回调函数）；并注册到 poller
    channel.reset(new Channel(_loop, _connFd, 0, 0, nullptr, nullptr, nullptr, nullptr));
    channel->enable_reading();
    channel->subscribe_on_read(std::bind(&TcpConnection::read_callback, this));
    channel->subscribe_on_write([this]() { this->write_callback(); });
    channel->subscribe_on_close([this]() { this->close_callback(); });
    channel->subscribe_on_error([this]() { this->error_callback(); });
    // channel->update_to_register();
}

TcpConnection::~TcpConnection() {
    // 虽然 connFd 是由 Acceptor 创建的，但是由 TcpConnection 持有，所以应该由其负责销毁
    int retClose = ::close(this->connectFd);
    assert(retClose != -1);
}

// Buffer* TcpConnection::get_input_buffer() {
//     return this->readBuffer.get();
// }

// Buffer* TcpConnection::get_output_buffer() {
//     return this->writeBuffer.get();
// }

void TcpConnection::read_callback() {
    int savedErrno = 0;
    ssize_t n = readBuffer->read_from_fd(this->connectFd, &savedErrno);
    if (n > 0) {
        this->handle_message();
    }
    else if (n == 0) {
        this->close_callback();
    }
    else {
        std::cerr << "Read error: " << savedErrno << std::endl;
        this->close_callback();
    }
}

void TcpConnection::write_callback() {
    int savedErrno = 0;
    ssize_t n = writeBuffer->write_to_fd(this->connectFd, &savedErrno);
    if (n > 0) {
        if (writeBuffer->readable_bytes() == 0) {
            channel->disable_writing();
        }
    }
    else {
        std::cerr << "Write error: " << savedErrno << std::endl;
    }
}

void TcpConnection::close_callback() {
    channel->disable_all();
    channel->remove_in_register();

    this->handle_close();
}

void TcpConnection::error_callback() {
    channel->disable_all();
    channel->remove_in_register();

    this->handle_close();
}

void TcpConnection::handle_message() {
    if (messageCallback) {
        messageCallback(shared_from_this());
    }
    else {
        LOG::LOG_ERROR("TcpConnection::handle_message(). no messageCallback setted.");
    }
}

void TcpConnection::handle_close() {
    if (closeCallback) {
        closeCallback(shared_from_this());
    }
    else LOG::LOG_ERROR("TcpConnection::handle_close(). no closeCallback setted.");
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