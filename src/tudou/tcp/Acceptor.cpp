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

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr) :
    loop_(loop),
    listenAddr_(listenAddr),
    listenFd_(-1),
    channel_(nullptr),
    newConnectCallback_(nullptr),
    acceptedConnFd_(-1),
    acceptedPeerAddr_("0.0.0.0", 0) {

    // 创建 listenFd
    this->listenFd_ = this->create_fd();
    this->bind_address(listenFd_);
    this->start_listen(listenFd_);

    // 初始化 channel（unique_ptr 无法拷贝，只能用 reset 或者 std::make_unique）
    this->channel_.reset(new Channel(loop_, listenFd_));
    this->channel_->set_read_callback(std::bind(&Acceptor::on_read, this, std::placeholders::_1));
    this->channel_->set_error_callback(std::bind(&Acceptor::on_error, this, std::placeholders::_1));
    this->channel_->set_close_callback(std::bind(&Acceptor::on_close, this, std::placeholders::_1));
    this->channel_->set_write_callback(std::bind(&Acceptor::on_write, this, std::placeholders::_1));
    this->channel_->enable_reading();
}

Acceptor::~Acceptor() {
    // 要么是栈上对象，要么是智能指针管理的堆对象，不需要手动 delete
}

void Acceptor::set_connect_callback(NewConnectCallback cb) {
    this->newConnectCallback_ = std::move(cb);
}

int Acceptor::get_accepted_fd() {
    if (this->acceptedConnFd_ < 0) {
        spdlog::error("Acceptor::get_accepted_fd() called but no accepted connection.");
        assert(false);
    }
    int fd = this->acceptedConnFd_;
    this->acceptedConnFd_ = -1; // 取出后重置，防止重复使用同一个 fd
    return fd;
}

const InetAddress& Acceptor::get_accepted_peer_addr() {
    return this->acceptedPeerAddr_; // TODO: 是否需要重置？（返回 const&，暂时不重置）
}

int Acceptor::get_listen_fd() const {
    return channel_->get_fd();
}

int Acceptor::create_fd() {
    // 创建 listenFd, 指定非阻塞 IO 套接字
    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP /* 0 */);
    if (listenFd_ < 0) {
        spdlog::error("Acceptor::create_fd(). socket error, errno: {}", errno);
        assert(false);
    }
    return listenFd_;
}

void Acceptor::bind_address(int listenFd) {
    sockaddr_in address = this->listenAddr_.get_sockaddr();
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
    // 理论上 Acceptor 不会触发 error、close、write 事件，只监听读事件（新连接到来）。
    // 但为了完整性，仍然预留这些回调接口处理逻辑
    spdlog::error("Acceptor::on_error() is called.");
    spdlog::error("Acceptor::listenFd {} error.", channel.get_fd());
}

void Acceptor::on_close(Channel& channel) {
    spdlog::error("Acceptor::on_close() is called.");
    spdlog::error("Acceptor::listenFd {} is closed.", channel.get_fd());
    assert(false);
}

void Acceptor::on_write(Channel& channel) {
    spdlog::error("Acceptor::on_write() is called.");
    spdlog::error("Acceptor::listenFd {} write event.", channel.get_fd());
}

void Acceptor::on_read(Channel& channel) {
    // TODO
    // 看 EpollPoller::update_channel() 可知默认是水平触发，所以不需要循环 accept，只有 fd 可读就会一直触发读事件
    // 但是 Acceptor::create_fd() 创建的 listenFd 使用非阻塞，所以可以循环 accept，可能一次读事件接收多个连接，提升性能
    // 循环 accept 的话，回调参数设计就不合适了，因为一次读事件可能接收多个连接（可以考虑 vector 保存创建的新连接）
    sockaddr_in clientAddr;
    socklen_t len = sizeof(clientAddr);
    int connFd = ::accept(listenFd_, (sockaddr*)&clientAddr, &len);
    if (connFd < 0) {
        spdlog::error("Acceptor::on_read(). accept error, errno: {}", errno);
        return; // ★ 失败一定要直接返回，不能继续执行后续逻辑（回调）
    }

    // 保存新连接信息到成员变量，供上层回调通过接口获取
    acceptedConnFd_ = connFd;
    acceptedPeerAddr_ = InetAddress(clientAddr);
    spdlog::debug("Acceptor::ConnectFd {} is accepted from {}.", acceptedConnFd_, acceptedPeerAddr_.get_ip_port());

    // 嵌套调用回调函数。触发上层回调，上层进行逻辑处理
    // 回调函数内部可以通过 get_accepted_fd() 和 get_accepted_peer_addr() 获取连接信息
    handle_connect_callback();
}

void Acceptor::handle_connect_callback() {
    assert(this->newConnectCallback_ != nullptr);
    this->newConnectCallback_(*this); // 传递 Acceptor 引用
}