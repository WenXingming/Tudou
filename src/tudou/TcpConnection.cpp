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

#include "../base/Log.h"
#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"

TcpConnection::TcpConnection(EventLoop* _loop, int _connFd)
    : loop(_loop), connectFd(_connFd) {

    // 初始化 channel. 创建 channel 后需要设置 intesting event 和 订阅（发生事件后的回调函数）
    channel.reset(new Channel(_loop, _connFd));
    channel->enable_reading();
    channel->set_read_callback(std::bind(&TcpConnection::read_callback, this));
    channel->set_write_callback([this]() { this->write_callback(); });
    channel->set_close_callback([this]() { this->close_callback(); });
    channel->set_error_callback([this]() { this->error_callback(); });
    channel->update_to_register(); // 注册到 poller，和 TcpConnection 创建同步

    // 初始化缓冲区, unique_ptr 自动管理内存
    readBuffer.reset(new Buffer());
    writeBuffer.reset(new Buffer());
}

TcpConnection::~TcpConnection() {
    channel->disable_all();
    channel->remove_in_register(); // 注销 channel，channels 和 TcpConnection 销毁同步
    int retClose = ::close(this->connectFd);
    assert(retClose != -1);
}

int TcpConnection::get_fd() const {
    return this->connectFd;
}

void TcpConnection::set_message_callback(MessageCallback _cb) {
    assert(_cb != nullptr);
    this->messageCallback = std::move(_cb);
}

void TcpConnection::set_close_callback(CloseCallback _cb) {
    assert(_cb != nullptr);
    this->closeCallback = std::move(_cb);
}

// 从 fd 读数据到 readBuffer，然后触发上层回调处理数据
void TcpConnection::read_callback() {
    int savedErrno = 0;
    ssize_t n = readBuffer->read_from_fd(this->connectFd, &savedErrno);
    if (n > 0) {
        this->handle_message();
    }
    else if (n == 0) { // 对端关闭
        this->close_callback();
    }
    else {
        LOG::LOG_ERROR("TcpConnection::read_callback(). read error: %d", savedErrno);
        this->close_callback();
    }
}

// 从 writeBuffer 写数据到 fd，写完了就取消对写事件的关注
void TcpConnection::write_callback() {
    int savedErrno = 0;
    ssize_t n = writeBuffer->write_to_fd(connectFd, &savedErrno);
    if (n > 0) {
        if (writeBuffer->readable_bytes() == 0) { // writeBuffer 里的数据写完了
            channel->disable_writing();
        }
    }
    else {
        std::cerr << "Write error: " << savedErrno << std::endl;
    }
}

void TcpConnection::close_callback() {
    channel->disable_all();
    // channel->remove_in_register();

    this->handle_close();
}

void TcpConnection::error_callback() {
    channel->disable_all();
    // channel->remove_in_register();

    this->handle_close();
}

void TcpConnection::handle_message() {
    assert(messageCallback != nullptr);
    messageCallback(shared_from_this());
}

void TcpConnection::handle_close() {
    assert(closeCallback != nullptr);
    closeCallback(shared_from_this());
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