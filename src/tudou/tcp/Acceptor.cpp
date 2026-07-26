// ============================================================================
// Acceptor 编排监听事件、接收新连接并向上层发布连接对象。
// ============================================================================

#include "tudou/tcp/Acceptor.h"

#include <cassert>
#include <cerrno>
#include <fcntl.h>

#include "spdlog/spdlog.h"
#include "tudou/reactor/Channel.h"
#include "tudou/reactor/EventLoop.h"

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr)
    : loop_(loop)
    , listenSocket_(Socket::create_tcp_listener(listenAddr))
    , channel_(std::make_unique<Channel>(loop_, listenSocket_.fd()))
    , idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))
    , newConnectCallback_(nullptr) {
    channel_->set_read_callback([this](Channel& ch) {
        on_read(ch);
        });
    channel_->enable_reading();
}

Acceptor::~Acceptor() = default;

void Acceptor::set_connect_callback(NewConnectCallback cb) {
    newConnectCallback_ = std::move(cb);
}

int Acceptor::get_listen_fd() const {
    return listenSocket_.fd();
}

void Acceptor::on_read(Channel& channel) {
    // listen fd 可读表示有新连接到来，accept 返回一个新 socket fd 和对端地址。
    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(clientAddr);
    if (connSocket.fd() < 0) {
        // EMFILE/ENFILE：fd 耗尽，内核队列中的挂起连接无法取出，会导致 epoll 持续触发 busy-loop。
        // 通过关闭预留的 idle fd 腾出名额、重试 accept 拉走挂起连接来打破循环。
        if (errno == EMFILE || errno == ENFILE) {
            accept_idle_connection();
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

void Acceptor::accept_idle_connection() {
    spdlog::error("Acceptor: fd exhausted (EMFILE/ENFILE), entering recovery");

    // 1. 关闭预留 fd，腾出一个名额。
    idleFd_ = Socket(-1);

    // 2. 接收并立即关闭一个挂起连接，避免监听 fd 持续触发。
    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(clientAddr);

    // 3. 重新打开 /dev/null，恢复 fd 预留。
    idleFd_ = Socket(::open("/dev/null", O_RDONLY | O_CLOEXEC));
}
