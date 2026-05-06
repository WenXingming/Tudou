// ============================================================================
// Acceptor.cpp
// 监听接入器实现，显式展开“监听、accept、发布新连接”的控制路径。
// ============================================================================

#include "Acceptor.h"

#include <cassert>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
    newConnectCallback_(nullptr) {

    listenFd_ = create_fd();
    bind_address(listenFd_);
    start_listen(listenFd_);

    channel_ = std::make_unique<Channel>(loop_, listenFd_);
    bind_channel_callbacks();
}

Acceptor::~Acceptor() {
}

void Acceptor::set_connect_callback(NewConnectCallback cb) {
    newConnectCallback_ = std::move(cb);
}

int Acceptor::get_listen_fd() const {
    return channel_->get_fd();
}

int Acceptor::create_fd() {
    const int listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listenFd < 0) {
        spdlog::error("Acceptor::create_fd(): socket error, errno: {}", errno);
        assert(false);
    }
    return listenFd;
}

void Acceptor::bind_address(int listenFd) {
    sockaddr_in address = listenAddr_.get_sockaddr();
    if (::bind(listenFd, (sockaddr*)&address, sizeof(address)) < 0) {
        spdlog::error("Acceptor::bind_address(): bind error, errno: {}", errno);
        assert(false);
    }
}

void Acceptor::start_listen(int listenFd) {
    if (::listen(listenFd, SOMAXCONN) < 0) {
        spdlog::error("Acceptor::start_listen(): listen error, errno: {}", errno);
        assert(false);
    }
}

void Acceptor::bind_channel_callbacks() {
    channel_->set_read_callback([this](Channel& ch) { on_read(ch); });
    channel_->set_error_callback([this](Channel& ch) { on_error(ch); });
    channel_->set_close_callback([this](Channel& ch) { on_close(ch); });
    channel_->set_write_callback([this](Channel& ch) { on_write(ch); });
    channel_->enable_reading();
}

void Acceptor::on_error(Channel& channel) {
    spdlog::error("Acceptor: listenFd {} error", channel.get_fd());
}

void Acceptor::on_close(Channel& channel) {
    spdlog::error("Acceptor: listenFd {} closed unexpectedly", channel.get_fd());
    assert(false);
}

void Acceptor::on_write(Channel& channel) {
    spdlog::error("Acceptor: unexpected write event on listenFd {}", channel.get_fd());
}

void Acceptor::on_read(Channel& channel) {
    // LT 模式下每次只接一个连接即可；若 backlog 仍有数据，epoll 会再次唤醒。
    sockaddr_in clientAddr{};
    const int connFd = accept_connection(&clientAddr);
    if (connFd < 0) {
        if (!is_transient_accept_error(errno)) {
            spdlog::error("Acceptor::on_read(): accept error, errno: {}", errno);
        }
        return;
    }

    publish_connection(connFd, clientAddr);
}

int Acceptor::accept_connection(sockaddr_in* clientAddr) const {
    socklen_t length = sizeof(*clientAddr);
    return ::accept4(listenFd_, reinterpret_cast<sockaddr*>(clientAddr), &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

bool Acceptor::is_transient_accept_error(int errorCode) const {
    return errorCode == EAGAIN || errorCode == EWOULDBLOCK || errorCode == EINTR;
}

void Acceptor::publish_connection(int connFd, const sockaddr_in& clientAddr) {
    const InetAddress peerAddr(clientAddr);
    spdlog::debug("Acceptor: connFd {} accepted from {}", connFd, peerAddr.get_ip_port());
    handle_connect_callback(connFd, peerAddr);
}

void Acceptor::handle_connect_callback(int connFd, const InetAddress& peerAddr) {
    assert(this->newConnectCallback_ != nullptr);
    newConnectCallback_(connFd, peerAddr); // 传递 connFd 和 peerAddr
}