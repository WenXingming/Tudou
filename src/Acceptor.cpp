/**
 * @file Acceptor.h
 * @brief 监听新连接的接入器（封装 listenFd 及持有其 Channel），在有连接到来时接受并上报给上层。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "Acceptor.h"
#include "EventLoop.h"
#include "Channel.h"
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <iostream>
#include <cassert>
#include "../base/Timestamp.h"
#include "../base/Log.h"


Acceptor::Acceptor(EventLoop* _loop, const InetAddress& _listenAddr, std::function<void(int)> _connectCallback) // 构造函数里访问 this 需要小心一些：有些成员变量没有在初始化列表里，是默认初始化
    : loop(_loop)
    , listenAddr(_listenAddr)
    , connectCallback(std::move(_connectCallback)) {

    // 初始化 this->listenFd
    this->create_fd();
    this->bind_address();
    this->start_listen();

    // 初始化 channel. 也可以放在初始化列表里，但注意初始化顺序（依赖 listenFd）。
    // 这里是 unique_ptr，channel 没有无参构造函数如果是对象则在初始化列表中无法初始化会编译失败
    // 注意：创建 channel 后需要设置 intesting event 和 订阅（发生事件后的回调函数）；并注册到 poller
    this->channel.reset(new Channel(this->loop, this->listenFd, 0, 0,
        nullptr, nullptr, nullptr, nullptr));
    this->channel->subscribe_on_read(std::bind(&Acceptor::read_callback, this));
    this->channel->enable_reading();
    // this->channel->update_to_register(); // 在构造函数、enable_xxx 内部调用了 update_to_register()，这样外部应该就不用调用了
}

Acceptor::~Acceptor() {
    assert(listenFd > 0);
    ::close(this->listenFd); // listenFd 生命期应该由 Acceptor 管理（创建和销毁）
}

void Acceptor::create_fd() {
    this->listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); // 创建非阻塞 socket
    assert(this->listenFd > 0);
}

void Acceptor::bind_address() {
    sockaddr_in address = this->listenAddr.get_sockaddr();
    int bindRet = ::bind(this->listenFd, (sockaddr*)&address, sizeof(address));
    assert(bindRet != -1);
}

void Acceptor::start_listen() {
    int listenRet = ::listen(this->listenFd, SOMAXCONN);
    assert(listenRet != -1);
}

void Acceptor::read_callback() {
    sockaddr_in clientAddr;
    socklen_t len = sizeof(clientAddr);
    int connFd = ::accept(this->listenFd, (sockaddr*)&clientAddr, &len);
    if (connFd >= 0) {
        LOG::LOG_DEBUG("ConnectFd %d is accepted.", connFd);
        publish_on_connect(connFd); // 发布给上层 TcpServer，TcpServer 根据 connFd 创建 TcpConnection
    }
    else {
        LOG::LOG_ERROR("Acceptor::handle_read(). accept error, errno: %d", errno);
    }
}

void Acceptor::subscribe_on_connect(std::function<void(int)> cb) {
    this->connectCallback = std::move(cb);
}

//@brief 发布新连接事件给上层 TcpServer, TcpServer 根据 connFd 创建 TcpConnection
void Acceptor::publish_on_connect(int connFd) {
    if (connectCallback) {
        connectCallback(connFd);
    }
    else {
        LOG::LOG_ERROR("Acceptor::publish_on_connect(). No connectCallback setted.");
        ::close(connFd);
    }
}