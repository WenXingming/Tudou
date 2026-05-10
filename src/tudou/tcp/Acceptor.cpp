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
    listenSocket_(Socket::create_tcp_listener(listenAddr)),
    channel_(nullptr),
    newConnectCallback_(nullptr) {

    // 监听 socket 就绪后再挂接 Channel，保证回调只面对可用的 listen fd
    channel_ = std::make_unique<Channel>(loop_, listenSocket_.fd());
    channel_->set_read_callback([this](Channel& ch) { on_read(ch); });
    channel_->enable_reading();
}

Acceptor::~Acceptor() {
}

void Acceptor::set_connect_callback(NewConnectCallback cb) {
    newConnectCallback_ = std::move(cb);
}

int Acceptor::get_listen_fd() const {
    return listenSocket_.fd();
}

void Acceptor::on_read(Channel& channel) {
    // listen fd 可读表示有新连接到来，accept 返回一个新 socket fd 和对端地址。
    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(&clientAddr);
    if (connSocket.fd() < 0) {
        // 瞬时错误无需诊断，直接忽略。
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            spdlog::error("Acceptor::on_read(): accept error, errno: {}", errno);
        }
        return;
    }

    InetAddress peerAddr(clientAddr);
    spdlog::debug("Acceptor: connFd {} accepted from {}", connSocket.fd(), peerAddr.get_ip_port());
    handle_connect_callback(std::move(connSocket), peerAddr);
}

void Acceptor::handle_connect_callback(Socket connSocket, const InetAddress& peerAddr) {
    assert(newConnectCallback_ != nullptr);
    newConnectCallback_(std::move(connSocket), peerAddr);
}
