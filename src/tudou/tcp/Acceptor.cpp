// ============================================================================
// Acceptor.cpp
// 监听接入器实现，Socket 接管底层 socket 操作，Acceptor 只做编排。
// ============================================================================

#include "Acceptor.h"

#include <cassert>
#include <cerrno>

#include "spdlog/spdlog.h"
#include "Channel.h"
#include "EventLoop.h"

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr) :
    loop_(loop),
    listenAddr_(listenAddr),
    listenSocket_(Socket::create_tcp_listener(listenAddr_)),
    channel_(nullptr),
    newConnectCallback_(nullptr) {

    // 监听 socket 就绪后再挂接 Channel，保证回调只面对可用的 listen fd
    channel_ = std::make_unique<Channel>(loop_, listenSocket_.fd());
    bind_channel_callbacks();
}

Acceptor::~Acceptor() {
}

void Acceptor::set_connect_callback(NewConnectCallback cb) {
    newConnectCallback_ = std::move(cb);
}

int Acceptor::get_listen_fd() const {
    return listenSocket_.fd();
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
    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(&clientAddr);
    if (connSocket.fd() < 0) {
        if (!is_transient_accept_error(errno)) {
            spdlog::error("Acceptor::on_read(): accept error, errno: {}", errno);
        }
        return;
    }

    publish_connection(std::move(connSocket), InetAddress(clientAddr));
}

bool Acceptor::is_transient_accept_error(int errorCode) const {
    return errorCode == EAGAIN || errorCode == EWOULDBLOCK || errorCode == EINTR;
}

void Acceptor::publish_connection(Socket connSocket, const InetAddress& peerAddr) {
    spdlog::debug("Acceptor: connFd {} accepted from {}", connSocket.fd(), peerAddr.get_ip_port());
    handle_connect_callback(std::move(connSocket), peerAddr);
}

void Acceptor::handle_connect_callback(Socket connSocket, const InetAddress& peerAddr) {
    assert(this->newConnectCallback_ != nullptr);
    newConnectCallback_(std::move(connSocket), peerAddr);
}
