/**
 * @file Acceptor.cpp
 * @brief 监听新连接的接入器
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "Acceptor.h"

#include <arpa/inet.h>
#include <cassert>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include "spdlog/spdlog.h"
#include "Channel.h"
#include "EventLoop.h"


namespace {

void apply_tcp_keepalive(int fd) {
    // TODO: 这些参数可以通过 Acceptor 的构造函数、TCPServer 的构造函数或者配置文件进行配置，目前先写死
    constexpr int kEnable = 1;
    constexpr int kKeepIdleSec = 60;
    constexpr int kKeepIntvlSec = 10;
    constexpr int kKeepCnt = 3;

    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("Acceptor: failed to enable SO_KEEPALIVE on fd {}, errno: {}", fd, errno);
        return;
    }

    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &kKeepIdleSec, sizeof(kKeepIdleSec)) < 0) {
        spdlog::warn("Acceptor: failed to set TCP_KEEPIDLE on fd {}, errno: {}", fd, errno);
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &kKeepIntvlSec, sizeof(kKeepIntvlSec)) < 0) {
        spdlog::warn("Acceptor: failed to set TCP_KEEPINTVL on fd {}, errno: {}", fd, errno);
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &kKeepCnt, sizeof(kKeepCnt)) < 0) {
        spdlog::warn("Acceptor: failed to set TCP_KEEPCNT on fd {}, errno: {}", fd, errno);
    }
}

} // namespace


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
    channel_->set_read_callback([this](Channel& ch) { on_read(ch); });
    channel_->set_error_callback([this](Channel& ch) { on_error(ch); });
    channel_->set_close_callback([this](Channel& ch) { on_close(ch); });
    channel_->set_write_callback([this](Channel& ch) { on_write(ch); });
    channel_->enable_reading();
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
    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listenFd_ < 0) {
        spdlog::error("Acceptor::create_fd(): socket error, errno: {}", errno);
        assert(false);
    }
    return listenFd_;
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
    // LT 模式下每次 accept 一个连接即可，fd 仍可读时 epoll 会再次触发
    sockaddr_in clientAddr;
    socklen_t len = sizeof(clientAddr);
    int connFd = ::accept(listenFd_, (sockaddr*)&clientAddr, &len);
    if (connFd < 0) {
        spdlog::error("Acceptor::on_read(): accept error, errno: {}", errno);
        return;
    }
    InetAddress peerAddr(clientAddr);
    spdlog::debug("Acceptor: connFd {} accepted from {}", connFd, peerAddr.get_ip_port());

    // 设置 TCP keepalive，检测死连接，及时释放资源。对于短连接来说可能不太必要，但对于长连接来说非常重要
    // 尤其是当客户端崩溃或者网络异常断开时，服务器端无法通过正常的连接关闭流程来获知连接已经不可用
    // 如果不启用 TCP keepalive，这些死连接就会一直占用服务器资源，直到服务器重启或者达到系统级的 TCP keepalive 超时才会被清理掉，这可能导致资源耗尽和性能问题
    apply_tcp_keepalive(connFd);

    // 嵌套调用回调函数。触发上层回调，上层进行逻辑处理
    handle_connect_callback(connFd, peerAddr);
}

void Acceptor::handle_connect_callback(int connFd, const InetAddress& peerAddr) {
    assert(this->newConnectCallback_ != nullptr);
    newConnectCallback_(connFd, peerAddr); // 传递 connFd 和 peerAddr
}