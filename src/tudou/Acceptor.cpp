/**
 * @file Acceptor.cpp
 * @brief 监听新连接的接入器（封装 listenFd 及持有其 Channel），在有连接到来时接受并上报给上层
 * @author wenxingming
 * @date 2025-12-16
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "Acceptor.h"

#include <arpa/inet.h>
#include <cassert>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "spdlog/spdlog.h"
#include "Channel.h"
#include "EventLoop.h"

Acceptor::Acceptor(EventLoop* _loop, const InetAddress& _listenAddr) :
    loop(_loop),
    listenAddr(_listenAddr),
    listenFd(-1),
    channel(nullptr),
    newConnectCallback(nullptr) {

    // 创建 listenFd
    listenFd = this->create_fd();
    this->bind_address(listenFd);
    this->start_listen(listenFd);

    // 初始化 channel
    this->channel.reset(new Channel(this->loop, listenFd)); // unique_ptr 无法拷贝，所以使用 reset + new
    this->channel->set_read_callback(std::bind(&Acceptor::on_read, this, std::placeholders::_1));
    this->channel->set_error_callback(std::bind(&Acceptor::on_error, this, std::placeholders::_1));
    this->channel->set_close_callback(std::bind(&Acceptor::on_close, this, std::placeholders::_1));
    this->channel->set_write_callback(std::bind(&Acceptor::on_write, this, std::placeholders::_1));
    this->channel->enable_reading();
}

Acceptor::~Acceptor() {

}

int Acceptor::get_listen_fd() const {
    return channel->get_fd();
}

void Acceptor::set_connect_callback(std::function<void(int)> _cb) {
    this->newConnectCallback = std::move(_cb);
}

int Acceptor::create_fd() {
    // 创建 listenFd, 指定非阻塞 IO 套接字
    listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP /* 0 */);
    if (listenFd < 0) {
        spdlog::error("Acceptor::create_fd(). socket error, errno: {}", errno);
        assert(false);
    }
    return listenFd;
}

void Acceptor::bind_address(int listenFd) {
    sockaddr_in address = this->listenAddr.get_sockaddr();
    int bindRet = ::bind(listenFd, (sockaddr*)&address, sizeof(address));
    if (bindRet == -1) {
        spdlog::error("Acceptor::bind_address(). bind error, errno: {}", errno);
        assert(false);
    }
}

void Acceptor::start_listen(int listenFd) {
    int listenRet = ::listen(listenFd, SOMAXCONN);
    if (listenRet == -1) {
        spdlog::error("Acceptor::start_listen(). listen error, errno: {}", errno);
        assert(false);
    }
}

void Acceptor::on_error(Channel& channel) {
    // 理论上不应该触发错误事件，但触发了也不应该崩溃
    spdlog::error("Acceptor::on_error() is called.");
    spdlog::error("Acceptor::listenFd {} error.", channel.get_fd());
}

void Acceptor::on_close(Channel& channel) {
    // 理论上不应该触发关闭事件
    spdlog::error("Acceptor::on_close() is called.");
    spdlog::error("Acceptor::listenFd {} is closed.", channel.get_fd());
    assert(false);
}

void Acceptor::on_write(Channel& channel) {
    // 理论上不应该触发写事件，但触发了也不应该崩溃
    spdlog::error("Acceptor::on_write() is called.");
    spdlog::error("Acceptor::listenFd {} write event.", channel.get_fd());
}

void Acceptor::on_read(Channel& channel) {
    sockaddr_in clientAddr;
    socklen_t len = sizeof(clientAddr);
    int connFd = ::accept(listenFd, (sockaddr*)&clientAddr, &len);
    if (connFd < 0) {
        spdlog::error("Acceptor::on_read(). accept error, errno: {}", errno);
        return; // ★ 失败一定要直接返回，不能继续执行后续逻辑（回调）
    }
    spdlog::info("Acceptor::ConnectFd {} is accepted.", connFd);

    // 嵌套调用回调函数。触发上层回调，上层进行逻辑处理
    handle_connect_callback(connFd);
}

void Acceptor::handle_connect_callback(int connFd) {
    assert(this->newConnectCallback != nullptr);
    this->newConnectCallback(connFd);
}