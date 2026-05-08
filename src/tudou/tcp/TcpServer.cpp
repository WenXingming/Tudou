// ============================================================================
// TcpServer.cpp
// TcpServer 的实现：Socket 沿回调链传递到 TcpConnection，沿途配置 socket 选项。
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
}

TcpServer::~TcpServer() {
}

void TcpServer::start() {
    spdlog::debug("TcpServer::start() called, starting server at {}:{}", ip_, port_);

    loopThreadPool_->start();
    EventLoop& mainLoop = *loopThreadPool_->get_main_loop();

    InetAddress listenAddr(ip_, port_);
    acceptor_ = std::make_unique<Acceptor>(&mainLoop, listenAddr);
    acceptor_->set_connect_callback([this](Socket connSocket, const InetAddress& peerAddr) {
        on_connect(std::move(connSocket), peerAddr);
    });

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

void TcpServer::assert_in_main_loop_thread() const {
    EventLoop* mainLoop = loopThreadPool_->get_main_loop();
    assert(mainLoop != nullptr);
    assert(mainLoop->is_in_loop_thread());
}

EventLoop& TcpServer::select_loop() const {
    EventLoop* ioLoop = loopThreadPool_->get_next_loop();
    if (ioLoop == nullptr) {
        spdlog::critical("TcpServer::select_loop(). ioLoop is nullptr.");
        assert(false);
    }
    return *ioLoop;
}

void TcpServer::on_connect(Socket connSocket, const InetAddress& peerAddr) {
    assert_in_main_loop_thread();

    const int fd = connSocket.fd();
    spdlog::info("TcpServer: New connection from {} on fd {}", peerAddr.get_ip_port(), fd);

    EventLoop* ioLoop = &select_loop();

    // Socket 是 move-only 类型，用 shared_ptr 包装使 lambda 可拷贝以适配 std::function。
    auto connSocketPtr = std::make_shared<Socket>(std::move(connSocket));

    ioLoop->run_in_loop([this, connSocketPtr, ioLoop, peerAddr, fd]() {
        // 在交给 TcpConnection 之前统一配置 socket 选项
        connSocketPtr->set_tcp_no_delay(true);
        connSocketPtr->set_keep_alive(true);
        const InetAddress localAddr = connSocketPtr->local_address();

        const auto conn = create_connection(*ioLoop, std::move(*connSocketPtr), localAddr, peerAddr);

        bind_connection_callbacks(conn);
        store_connection(fd, conn);
        establish_connection(conn);
        notify_connection_callback(conn);
        });
}

std::shared_ptr<TcpConnection> TcpServer::create_connection(EventLoop& ioLoop,
    Socket connSocket,
    const InetAddress& localAddr,
    const InetAddress& peerAddr) const {
    return std::make_shared<TcpConnection>(&ioLoop, std::move(connSocket), localAddr, peerAddr);
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

void TcpServer::store_connection(int fd, const std::shared_ptr<TcpConnection>& conn) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_[fd] = conn;
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
    notify_message_callback(conn);
}

void TcpServer::notify_message_callback(const std::shared_ptr<TcpConnection>& conn) {
    assert(this->messageCallback_ != nullptr);
    this->messageCallback_(conn);
}

void TcpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    remove_connection(conn);
    notify_close_callback(conn);
}

void TcpServer::remove_connection(const std::shared_ptr<TcpConnection>& conn) {
    EventLoop* loop = conn->get_loop();
    assert(loop->is_in_loop_thread());

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
