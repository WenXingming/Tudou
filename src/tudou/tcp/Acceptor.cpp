// ============================================================================
// Acceptor.cpp
// 监听接入器实现，Socket 接管底层 socket 操作，Acceptor 只做编排。
// ============================================================================

#include "tudou/tcp/Acceptor.h"

#include <cassert>
#include <cerrno>
#include <fcntl.h>

#include "spdlog/spdlog.h"
#include "tudou/reactor/Channel.h"
#include "tudou/reactor/EventLoop.h"

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr) :
    loop_(loop),
    listenSocket_(Socket::create_tcp_listener(listenAddr)),
    channel_(nullptr),
    newConnectCallback_(nullptr) {

    // 监听 socket 就绪后再挂接 Channel，保证回调只面对可用的 listen fd
    channel_ = std::make_unique<Channel>(loop_, listenSocket_.fd());
    channel_->set_read_callback([this](Channel& ch) {
        on_read(ch);
        });
    channel_->enable_reading();

    // 预留一个空闲 fd（/dev/null），fd 耗尽时关闭它腾出名额重试 accept，防 busy-loop。
    idleFd_ = Socket(::open("/dev/null", O_RDONLY | O_CLOEXEC));
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

void Acceptor::accept_idle_connection() {
    spdlog::error("Acceptor: fd exhausted (EMFILE/ENFILE), entering recovery");

    // 1. 关闭 idle fd 腾出 1 个 fd 名额。
    idleFd_ = Socket(-1);

    // 2. 重试 accept 拉走内核队列中的挂起连接，因作用域结束自动关闭，通知客户端连接重置。
    sockaddr_in clientAddr{};
    {
        Socket connSocket = listenSocket_.accept(&clientAddr);
    }

    // 3. 重新打开 /dev/null 恢复占位，用于下一次 EMFILE 恢复。
    idleFd_ = Socket(::open("/dev/null", O_RDONLY | O_CLOEXEC));
}

void Acceptor::handle_connect_callback(Socket connSocket, const InetAddress& peerAddr) {
    assert(newConnectCallback_ != nullptr);
    newConnectCallback_(std::move(connSocket), peerAddr);
}
