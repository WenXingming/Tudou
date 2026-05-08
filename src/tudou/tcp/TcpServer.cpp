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
#include "ConnectionHeartbeat.h"
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
    connectionHeartbeats_(),
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
        const auto conn = create_connection(*ioLoop, std::move(*connSocketPtr), peerAddr, fd);

        handle_connection_callback(conn);
        });
}

std::shared_ptr<TcpConnection> TcpServer::create_connection(EventLoop& ioLoop,
    Socket connSocket,
    const InetAddress& peerAddr,
    int fd) {
    const InetAddress localAddr = connSocket.local_address();
    auto conn = std::make_shared<TcpConnection>(&ioLoop, std::move(connSocket), localAddr, peerAddr);

    // Channel tie 延迟到此处：TcpConnection 构造时 shared_from_this() 不可用，
    // 必须由 TcpServer 在 shared_ptr 管理对象后建立 weak tie，防止回调期间 owner 被销毁。
    conn->tie_to_object(conn); // 将 TcpConnection 的 shared_ptr 传入自身，建立弱引用，防止回调期间被销毁

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
            handle_error_callback(activeConn);
            });
    }

    if (writeCompleteCallback_) {
        conn->set_write_complete_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
            handle_write_complete_callback(activeConn);
            });
    }

    if (highWaterMarkCallback_) {
        conn->set_high_water_mark_callback([this](const std::shared_ptr<TcpConnection>& activeConn) {
            handle_high_water_mark_callback(activeConn);
            }, highWaterMark_);
    }

    // 根据服务器级默认配置，为这条连接按需创建独立的空闲检测器。
    std::shared_ptr<ConnectionHeartbeat> heartbeat = create_connection_heartbeat(conn);

    // 连接表和心跳表共用 fd 作为索引；两者统一由 TcpServer 在关闭路径上清理。
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_[fd] = conn;
        if (heartbeat) {
            connectionHeartbeats_[fd] = heartbeat;
        }
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


void TcpServer::handle_connection_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->connectionCallback_ == nullptr) {
        spdlog::warn("TcpServer::handle_connection_callback(). connectionCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->connectionCallback_(conn);
}

void TcpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    // 任何成功上浮到 TcpServer 的读事件都视为连接仍然活跃，因此先刷新空闲窗口再继续协议分发。
    refresh_connection_heartbeat(conn);
    handle_message_callback(conn);
}

void TcpServer::refresh_connection_heartbeat(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn) {
        return;
    }

    std::shared_ptr<ConnectionHeartbeat> heartbeat;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connectionHeartbeats_.find(conn->get_fd());
        if (it != connectionHeartbeats_.end()) {
            heartbeat = it->second;
        }
    }

    if (heartbeat) {
        heartbeat->refresh();
    }
}

void TcpServer::handle_message_callback(const std::shared_ptr<TcpConnection>& conn) {
    assert(this->messageCallback_ != nullptr);
    this->messageCallback_(conn);
}

void TcpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    remove_connection(conn);
    handle_close_callback(conn);
}

void TcpServer::remove_connection(const std::shared_ptr<TcpConnection>& conn) {
    assert(conn->get_loop()->is_in_loop_thread()); // 理论上应该总在 ioLoop 线程调用，assert 快速 Debug 代码可能的错误

    const int fd = conn->get_fd();
    std::shared_ptr<ConnectionHeartbeat> heartbeat;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        size_t erased = connections_.erase(fd); // 若找不到 fd 则 erase 返回 0，调用者可据此判断是否成功删除连接
        if (erased == 0) {
            spdlog::error("TcpServer::remove_connection(). connection not found, fd: {}", fd);
        }

        // 先从映射表摘掉，再在锁外 stop，避免定时器取消逻辑把临界区拖长。
        auto heartbeatIt = connectionHeartbeats_.find(fd);
        if (heartbeatIt != connectionHeartbeats_.end()) {
            heartbeat = heartbeatIt->second;
            connectionHeartbeats_.erase(heartbeatIt);
        }
    }

    if (heartbeat) {
        heartbeat->stop();
    }
}

void TcpServer::handle_close_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->closeCallback_ == nullptr) {
        spdlog::warn("TcpServer::handle_close_callback(). closeCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->closeCallback_(conn);
}

void TcpServer::handle_error_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->errorCallback_ == nullptr) {
        spdlog::warn("TcpServer::handle_error_callback(). errorCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->errorCallback_(conn);
}

void TcpServer::handle_write_complete_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->writeCompleteCallback_ == nullptr) {
        spdlog::warn("TcpServer::handle_write_complete_callback(). writeCompleteCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->writeCompleteCallback_(conn);
}

void TcpServer::handle_high_water_mark_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->highWaterMarkCallback_ == nullptr) {
        spdlog::warn("TcpServer::handle_high_water_mark_callback(). highWaterMarkCallback is nullptr, fd: {}", conn->get_fd());
        return;
    }
    this->highWaterMarkCallback_(conn);
}
