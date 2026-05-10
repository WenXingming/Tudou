// ============================================================================
// TcpServer.cpp
// TcpServer 的实现：Socket 沿回调链传递到 TcpConnection，沿途配置 socket 选项。
// ============================================================================

#include "TcpServer.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "base/InetAddress.h"
#include "spdlog/spdlog.h"
#include "Acceptor.h"
#include "ConnectionHeartbeat.h"
#include "EventLoop.h"
#include "TcpConnection.h"
#include "EventLoopThread.h"
#include "EventLoopThreadPool.h"

namespace {

constexpr size_t kDefaultHighWaterMark = 64 * 1024 * 1024; // 64 MB

} // namespace

TcpServer::TcpServer(std::string ip, uint16_t port, size_t ioLoopNum) :
    loopThreadPool_(nullptr),
    ioLoopNum_(ioLoopNum),
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
    highWaterMark_(kDefaultHighWaterMark),
    connectionHeartbeatOptions_() {
}

TcpServer::~TcpServer() {
}

void TcpServer::start() {
    spdlog::debug("TcpServer::start() called, starting server at {}:{}", ip_, port_);

    assert(loopThreadPool_ == nullptr);
    loopThreadPool_ = std::make_unique<EventLoopThreadPool>("TcpServerLoopPool", static_cast<int>(ioLoopNum_));
    loopThreadPool_->start();
    EventLoop& mainLoop = *loopThreadPool_->get_main_loop();

    InetAddress listenAddr(ip_, port_);
    acceptor_ = std::make_unique<Acceptor>(&mainLoop, listenAddr);
    acceptor_->set_connect_callback([this](Socket connSocket, const InetAddress& peerAddr) {
        on_connect(std::move(connSocket), peerAddr);
        });

    mainLoop.loop();

    shutdown_connections();
    acceptor_.reset();
    loopThreadPool_.reset();
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

void TcpServer::set_connection_heartbeat(double checkIntervalSeconds, double idleTimeoutSeconds) {
    if (checkIntervalSeconds <= 0.0 || idleTimeoutSeconds <= 0.0) {
        spdlog::warn("TcpServer::set_connection_heartbeat() invalid args, checkInterval={}, idleTimeout={}",
            checkIntervalSeconds,
            idleTimeoutSeconds);
        // 非法配置直接退化为关闭该功能，避免残留半初始化策略影响后续新连接。
        connectionHeartbeatOptions_ = ConnectionHeartbeatOptions();
        return;
    }

    // TcpServer 只保存默认策略参数，真正的 ConnectionHeartbeat 在每条连接创建时单独实例化。
    connectionHeartbeatOptions_.enabled = true;
    connectionHeartbeatOptions_.checkIntervalSeconds = checkIntervalSeconds;
    connectionHeartbeatOptions_.idleTimeoutSeconds = idleTimeoutSeconds;
}

void TcpServer::on_connect(Socket connSocket, const InetAddress& peerAddr) {
    EventLoop* mainLoop = loopThreadPool_->get_main_loop();
    assert(mainLoop != nullptr);
    assert(mainLoop->is_in_loop_thread());

    const int fd = connSocket.fd();
    spdlog::info("TcpServer: New connection from {} on fd {}", peerAddr.get_ip_port(), fd);

    EventLoop* ioLoop = loopThreadPool_->get_next_loop();
    assert(ioLoop != nullptr);

    // Socket 是 move-only 类型，用 shared_ptr 包装使 lambda 可拷贝以适配 std::function。
    auto connSocketPtr = std::make_shared<Socket>(std::move(connSocket));
    // 将新连接的 Socket 所有权转移到 ioLoop 线程，ioLoop 线程负责创建 TcpConnection 和 Channel，并管理其生命周期。
    ioLoop->run_in_loop([this, connSocketPtr, ioLoop, peerAddr, fd]() {
        const auto conn = create_connection(*ioLoop, std::move(*connSocketPtr), peerAddr);
        if (!connectionCallback_) {
            spdlog::warn("TcpServer::on_connect(). connectionCallback is nullptr, fd: {}", fd);
            return;
        }

        connectionCallback_(conn);
        });
}

std::shared_ptr<TcpConnection> TcpServer::create_connection(EventLoop& ioLoop,
    Socket connSocket,
    const InetAddress& peerAddr) {
    const InetAddress localAddr = connSocket.local_address();
    auto conn = TcpConnection::create(&ioLoop, std::move(connSocket), localAddr, peerAddr);
    const int fd = conn->get_fd();

    // 配置 TCP 选项，开启 TCP_NODELAY 和 TCP keepalive。
    conn->set_tcp_no_delay(true);
    conn->set_keep_alive(true);

    // 配置 TcpConnection 回调，全部转发到 TcpServer 以统一管理。
    conn->set_message_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
        on_message(activeConn);
        });
    conn->set_close_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
        on_close(activeConn);
        });

    if (errorCallback_) {
        conn->set_error_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
            errorCallback_(activeConn);
            });
    }

    if (writeCompleteCallback_) {
        conn->set_write_complete_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
            writeCompleteCallback_(activeConn);
            });
    }

    if (highWaterMarkCallback_) {
        conn->set_high_water_mark_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
            highWaterMarkCallback_(activeConn);
            }, highWaterMark_);
    }

    // 根据服务器级默认配置，为这条连接按需创建独立的空闲检测器。
    std::shared_ptr<ConnectionHeartbeat> heartbeat = create_connection_heartbeat(conn);

    // 连接和该连接的可选心跳策略一起注册和清理，避免生命周期分散到多张表。
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_[fd] = ConnectionEntry{ conn, heartbeat };
    }

    if (heartbeat) {
        heartbeat->start();
    }

    return conn;
}

std::shared_ptr<ConnectionHeartbeat> TcpServer::create_connection_heartbeat(const std::shared_ptr<TcpConnection>& conn) const {
    if (!connectionHeartbeatOptions_.enabled) {
        return nullptr;
    }

    // 策略对象只依赖 TcpConnection 的公共动作：refresh 通过 on_message 驱动，超时后调用 force_close。
    return std::make_shared<ConnectionHeartbeat>(conn,
        connectionHeartbeatOptions_.checkIntervalSeconds,
        connectionHeartbeatOptions_.idleTimeoutSeconds);
}

void TcpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    // 任何成功上浮到 TcpServer 的读事件都视为连接仍然活跃，因此先刷新空闲窗口再继续协议分发。
    refresh_connection_heartbeat(conn);
    if (!messageCallback_) {
        spdlog::warn("TcpServer::on_message(). messageCallback is nullptr, fd: {}", conn ? conn->get_fd() : -1);
        return;
    }

    messageCallback_(conn);
}

void TcpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    remove_connection(conn);
    if (!closeCallback_) {
        spdlog::warn("TcpServer::on_close(). closeCallback is nullptr, fd: {}", conn ? conn->get_fd() : -1);
        return;
    }

    closeCallback_(conn);
}

void TcpServer::remove_connection(const std::shared_ptr<TcpConnection>& conn) {
    assert(conn->get_loop()->is_in_loop_thread()); // 理论上应该总在 ioLoop 线程调用，assert 快速 Debug 代码可能的错误

    const int fd = conn->get_fd();
    std::shared_ptr<ConnectionHeartbeat> heartbeat;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            spdlog::error("TcpServer::remove_connection(). connection not found, fd: {}", fd);
        }
        else {
            heartbeat = it->second.heartbeat;
            connections_.erase(it);
        }
    }

    if (heartbeat) {
        heartbeat->stop();
    }
}

void TcpServer::refresh_connection_heartbeat(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn) {
        return;
    }

    std::shared_ptr<ConnectionHeartbeat> heartbeat;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(conn->get_fd());
        if (it != connections_.end()) {
            heartbeat = it->second.heartbeat;
        }
    }

    if (heartbeat) {
        heartbeat->refresh();
    }
}

void TcpServer::shutdown_connections() {
    std::vector<std::shared_ptr<TcpConnection>> activeConnections;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        activeConnections.reserve(connections_.size());
        for (const auto& entry : connections_) {
            if (entry.second.connection) {
                activeConnections.push_back(entry.second.connection);
            }
        }
    }

    for (const auto& conn : activeConnections) {
        conn->force_close();
    }

    while (true) {
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            if (connections_.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
