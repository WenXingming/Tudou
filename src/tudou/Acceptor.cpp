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
    // callback 参数使用 std::bind 绑定成员函数和 this 指针，函数被调用时（底层）参数的实际传入由底层传入（自身引用）
    this->channel.reset(new Channel(this->loop, listenFd)); // unique_ptr 无法拷贝，所以使用 reset + new
    this->channel->set_read_callback(std::bind(&Acceptor::on_read, this, std::placeholders::_1));
    this->channel->set_error_callback(std::bind(&Acceptor::on_error, this, std::placeholders::_1));
    this->channel->set_close_callback(std::bind(&Acceptor::on_close, this, std::placeholders::_1));
    this->channel->set_write_callback(std::bind(&Acceptor::on_write, this, std::placeholders::_1));
    this->channel->enable_reading();
    // this->channel->update_in_register(); // 注册到 poller，和 fd 创建同步

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

void Acceptor::on_error(Channel& channel) {
    // 在函数体里，同名的形参 channel 会屏蔽掉成员变量 channel；想访问成员变量，需要写成 this->channel
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
    int fd = channel.get_fd();
    sockaddr_in clientAddr;
    socklen_t len = sizeof(clientAddr);
    int connFd = ::accept(fd, (sockaddr*)&clientAddr, &len);
    if (connFd < 0) {
        spdlog::error("Acceptor::on_read(). accept error, errno: {}", errno);
    }
    spdlog::info("Acceptor::ConnectFd {} is accepted.", connFd);

    // 嵌套调用回调函数。触发上层回调，上层进行逻辑处理
    handle_connect_callback(connFd);
}

void Acceptor::handle_connect_callback(int connFd) {
    // 回调函数的又一个特点：参数由底层传入，逻辑由上层实现
    assert(this->connectCallback != nullptr);
    this->connectCallback(connFd);
}