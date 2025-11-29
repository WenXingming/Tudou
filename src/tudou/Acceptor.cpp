#include "Acceptor.h"

#include <arpa/inet.h>
#include <cassert>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../base/Log.h"
#include "Channel.h"
#include "EventLoop.h"

Acceptor::Acceptor(EventLoop* _loop, const InetAddress& _listenAddr) : loop(_loop), listenAddr(_listenAddr) {
    // 初始化 this->listenFd
    this->create_fd();
    this->bind_address();
    this->start_listen();

    // 初始化 channel. 也可以放在初始化列表里，但注意初始化顺序（依赖 listenFd）
    this->channel.reset(new Channel(this->loop, this->listenFd)); // unique_ptr 无法拷贝，所以使用 reset + new
    this->channel->enable_reading();
    this->channel->set_read_callback(std::bind(&Acceptor::read_callback, this));
    this->channel->update_to_register(); // 注册到 poller，和 fd 创建同步

    // connectCallback 已经在声明时初始化为 nullptr
}

Acceptor::~Acceptor() {
    this->channel->disable_all();
    this->channel->remove_in_register(); // 注销 channel，channels 和 fd 销毁同步

    assert(listenFd > 0);
    ::close(this->listenFd); // listenFd 生命期应该由 Acceptor 管理（创建和销毁）
}

int Acceptor::get_listen_fd() const {
    return this->listenFd;
}

void Acceptor::set_connect_callback(std::function<void(int)> cb) {
    this->connectCallback = std::move(cb);
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
        // LOG::LOG_DEBUG("Acceptor::ConnectFd %d is accepted.", connFd); // wrk 测试时注释掉
        handle_connect(connFd);
    }
    else {
        LOG::LOG_ERROR("Acceptor::handle_read(). accept error, errno: %d", errno);
    }
}

void Acceptor::handle_connect(int connFd) {
    // 回调函数的又一个特点：参数由底层传入，逻辑由上层实现
    assert(this->connectCallback != nullptr);
    this->connectCallback(connFd);
}