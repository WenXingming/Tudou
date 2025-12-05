#include "Acceptor.h"

#include <arpa/inet.h>
#include <cassert>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "spdlog/spdlog.h"
#include "Channel.h"
#include "EventLoop.h"

Acceptor::Acceptor(EventLoop* _loop, const InetAddress& _listenAddr)
    : loop(_loop)
    , listenAddr(_listenAddr) {

    // 初始化 this->listenFd
    int listenFd = this->create_fd();
    this->bind_address(listenFd);
    this->start_listen(listenFd);

    // 初始化 channel. 也可以放在初始化列表里，但注意初始化顺序（依赖 listenFd）
    this->channel.reset(new Channel(this->loop, listenFd)); // unique_ptr 无法拷贝，所以使用 reset + new
    this->channel->set_read_callback(std::bind(&Acceptor::read_callback, this));
    this->channel->set_error_callback(std::bind(&Acceptor::error_callback, this));
    this->channel->set_close_callback(std::bind(&Acceptor::close_callback, this));
    this->channel->set_write_callback([this]() { this->write_callback(); });
    this->channel->enable_reading();
    this->channel->update_to_register(); // 注册到 poller，和 fd 创建同步

    // connectCallback 已经在类声明时初始化为 nullptr。无需在此重复初始化
}

Acceptor::~Acceptor() {

}

int Acceptor::get_listen_fd() const {
    return this->channel->get_fd();
}

void Acceptor::set_connect_callback(std::function<void(int)> cb) {
    this->connectCallback = std::move(cb);
}

int Acceptor::create_fd() {
    int listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); // 创建非阻塞 socket
    assert(listenFd > 0);
    return listenFd;
}

void Acceptor::bind_address(int listenFd) {
    sockaddr_in address = this->listenAddr.get_sockaddr();
    int bindRet = ::bind(listenFd, (sockaddr*)&address, sizeof(address));
    assert(bindRet != -1);
}

void Acceptor::start_listen(int listenFd) {
    int listenRet = ::listen(listenFd, SOMAXCONN);
    assert(listenRet != -1);
}

void Acceptor::error_callback() {
    // 理论上不应该触发错误事件，但触发了也不应该崩溃
    spdlog::error("Acceptor::error_callback() is called.");
    spdlog::error("Acceptor::listenFd {} error.", this->channel->get_fd());
}

void Acceptor::close_callback() {
    // 理论上不应该触发关闭事件
    spdlog::error("Acceptor::close_callback() is called.");
    spdlog::error("Acceptor::listenFd {} is closed.", this->channel->get_fd());
    assert(false);
}

void Acceptor::write_callback() {
    // 理论上不应该触发写事件，但触发了也不应该崩溃
    spdlog::error("Acceptor::write_callback() is called.");
    spdlog::error("Acceptor::listenFd {} write event.", this->channel->get_fd());
}

void Acceptor::read_callback() {
    sockaddr_in clientAddr;
    socklen_t len = sizeof(clientAddr);
    int connFd = ::accept(this->channel->get_fd(), (sockaddr*)&clientAddr, &len);
    if (connFd < 0) {
        spdlog::error("Acceptor::read_callback(). accept error, errno: {}", errno);
    }

    // wrk 测试时注释掉日志，避免影响性能测试结果
    spdlog::debug("Acceptor::ConnectFd {} is accepted.", connFd);

    // 嵌套调用回调函数。触发上层回调，上层进行逻辑处理
    this->handle_connect(connFd);
}

void Acceptor::handle_connect(int connFd) {
    // 回调函数的又一个特点：参数由底层传入，逻辑由上层实现
    assert(this->connectCallback != nullptr);
    this->connectCallback(connFd);
}