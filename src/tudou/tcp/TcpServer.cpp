// ============================================================================
// TcpServer.cpp
// TcpServer 的实现保持单层编排：选择线程、装配连接、注册连接、通知上层。
// ============================================================================

#include "TcpServer.h"

#include <cassert>
#include <cerrno>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/InetAddress.h"
#include "spdlog/spdlog.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include "TcpConnection.h"
#include "EventLoopThread.h"
#include "EventLoopThreadPool.h"

namespace {

constexpr size_t kDefaultHighWaterMark = 64 * 1024 * 1024;

} // namespace

TcpServer::TcpServer(std::string ip, uint16_t port, size_t ioLoopNum) :
    loopThreadPool_(std::make_unique<EventLoopThreadPool>("TcpServerLoopPool", ioLoopNum)),
    ip_(std::move(ip)),
    port_(port),
    acceptor_(nullptr),
    connections_(),
    connectionsMutex_(),
    connectionCallback_(nullptr),
    messageCallback_(nullptr),
    closeCallback_(nullptr),
    errorCallback_(nullptr),
    writeCompleteCallback_(nullptr),
    highWaterMarkCallback_(nullptr),
    highWaterMark_(kDefaultHighWaterMark) {
    InetAddress listenAddr(this->ip_, this->port_);
    EventLoop& mainLoop = require_main_loop();

    acceptor_ = std::make_unique<Acceptor>(&mainLoop, listenAddr);
    acceptor_->set_connect_callback([this](int connFd, const InetAddress& peerAddr) {
        on_connect(connFd, peerAddr);
        });
}

TcpServer::~TcpServer() {
}

void TcpServer::start() {
    spdlog::debug("TcpServer::start() called, starting server at {}:{}", ip_, port_);

    EventLoop& mainLoop = require_main_loop();

    // 先启动 IO 线程池，再进入主循环，保证 accept 之后可以立刻把连接分发出去。
    loopThreadPool_->start();
    mainLoop.loop();
}

void TcpServer::set_connection_callback(ConnectionCallback cb) {
    this->connectionCallback_ = std::move(cb);
}

void TcpServer::set_message_callback(MessageCallback cb) {
    this->messageCallback_ = std::move(cb);
}

void TcpServer::set_close_callback(CloseCallback cb) {
    this->closeCallback_ = std::move(cb);
}

void TcpServer::set_error_callback(ErrorCallback cb) {
    this->errorCallback_ = std::move(cb);
}

void TcpServer::set_write_complete_callback(WriteCompleteCallback cb) {
    this->writeCompleteCallback_ = std::move(cb);
}

void TcpServer::set_high_water_mark_callback(HighWaterMarkCallback cb, size_t _highWaterMark) {
    this->highWaterMarkCallback_ = std::move(cb);
    this->highWaterMark_ = _highWaterMark;
}

EventLoop& TcpServer::require_main_loop() const {
    EventLoop* mainLoop = loopThreadPool_->get_main_loop();
    if (mainLoop == nullptr) {
        spdlog::critical("TcpServer::require_main_loop(). mainLoop is nullptr.");
        assert(false);
    }
    return *mainLoop;
}

void TcpServer::assert_in_main_loop_thread() const {
    require_main_loop().assert_in_loop_thread();
}

EventLoop& TcpServer::select_loop() const {
    EventLoop* ioLoop = loopThreadPool_->get_next_loop();
    if (ioLoop == nullptr) {
        spdlog::critical("TcpServer::select_loop(). ioLoop is nullptr.");
        assert(false);
    }
    return *ioLoop;
}

void TcpServer::on_connect(int connFd, const InetAddress& peerAddr) {
    assert_in_main_loop_thread();

    spdlog::info("TcpServer: New connection from {} on fd {}", peerAddr.get_ip_port(), connFd);

    EventLoop* ioLoop = &select_loop();

    // 连接必须在目标 IO 线程内一次性完成装配，避免回调早于初始化生效。
    ioLoop->run_in_loop([this, connFd, ioLoop, peerAddr]() {
        const InetAddress localAddr = resolve_local_address(connFd);
        const auto conn = create_connection(*ioLoop, connFd, localAddr, peerAddr);

        configure_connection_socket(conn);
        bind_connection_callbacks(conn);
        store_connection(connFd, conn);
        establish_connection(conn);
        notify_connection_callback(conn);
        });
}

InetAddress TcpServer::resolve_local_address(int connFd) const {
    sockaddr_in localSockAddr{};
    socklen_t addrLen = sizeof(localSockAddr);
    if (::getsockname(connFd, reinterpret_cast<sockaddr*>(&localSockAddr), &addrLen) < 0) {
        spdlog::error("TcpServer::resolve_local_address(). getsockname error, errno: {}", errno);
    }
    return InetAddress(localSockAddr);
}

std::shared_ptr<TcpConnection> TcpServer::create_connection(EventLoop& ioLoop,
    int connFd,
    const InetAddress& localAddr,
    const InetAddress& peerAddr) const {
    return std::make_shared<TcpConnection>(&ioLoop, connFd, localAddr, peerAddr);
}

void TcpServer::configure_connection_socket(const std::shared_ptr<TcpConnection>& conn) const {
    // 低延迟连接统一关闭 Nagle，同时启用 keepalive 及时回收死连接。
    conn->set_tcp_no_delay(true);
    conn->set_tcp_keepalive(true);
}

void TcpServer::bind_connection_callbacks(const std::shared_ptr<TcpConnection>& conn) {
    conn->set_message_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
        on_message(activeConn);
        });
    conn->set_close_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
        on_close(activeConn);
        });

    if (errorCallback_) {
        conn->set_error_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
            notify_error_callback(activeConn);
            });
    }

    if (writeCompleteCallback_) {
        conn->set_write_complete_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
            notify_write_complete_callback(activeConn);
            });
    }

    if (highWaterMarkCallback_) {
        conn->set_high_water_mark_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
            notify_high_water_mark_callback(activeConn);
            }, highWaterMark_);
    }
}

void TcpServer::store_connection(int connFd, const std::shared_ptr<TcpConnection>& conn) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_[connFd] = conn;
}

void TcpServer::establish_connection(const std::shared_ptr<TcpConnection>& conn) const {
    conn->connection_establish();
}

void TcpServer::notify_connection_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->connectionCallback_ == nullptr) {
        spdlog::warn("TcpServer::notify_connection_callback(). connectionCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->connectionCallback_(conn);
}

void TcpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    // TcpServer 只做事件转发，消息内容仍由业务层通过 conn->receive() 拉取。
    notify_message_callback(conn);
}

void TcpServer::notify_message_callback(const std::shared_ptr<TcpConnection>& conn) {
    assert(this->messageCallback_ != nullptr);
    this->messageCallback_(conn);
}

void TcpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    remove_connection(conn);

    // 先移除连接，再把关闭事件交给上层，避免回调期间看到陈旧连接表。
    notify_close_callback(conn);
}

void TcpServer::remove_connection(const std::shared_ptr<TcpConnection>& conn) {
    EventLoop* loop = conn->get_loop();
    loop->assert_in_loop_thread();

    int fd = conn->get_fd();
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto findIt = connections_.find(fd);
        if (findIt != connections_.end()) {
            connections_.erase(findIt);
            erased = true;
        }
    }

    if (!erased) {
        spdlog::error("TcpServer::remove_connection(). connection not found, fd: {}", fd);
    }
    else {
        spdlog::info("TcpServer::remove_connection(). connection removed, fd: {}", fd);
    }
}

void TcpServer::notify_close_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->closeCallback_ == nullptr) {
        spdlog::warn("TcpServer::notify_close_callback(). closeCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->closeCallback_(conn);
}

void TcpServer::notify_error_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->errorCallback_ == nullptr) {
        spdlog::warn("TcpServer::notify_error_callback(). errorCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->errorCallback_(conn);
}

void TcpServer::notify_write_complete_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->writeCompleteCallback_ == nullptr) {
        spdlog::warn("TcpServer::notify_write_complete_callback(). writeCompleteCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->writeCompleteCallback_(conn);
}

void TcpServer::notify_high_water_mark_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->highWaterMarkCallback_ == nullptr) {
        spdlog::warn("TcpServer::notify_high_water_mark_callback(). highWaterMarkCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->highWaterMarkCallback_(conn);
}
