// ============================================================================
// TcpServer.cpp
// TcpServer 的实现：Socket 沿回调链传递到 TcpConnection，沿途配置 socket 选项。
// ============================================================================

#include "TcpServer.h"

#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

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
    connectionRecordsByLoop_(),
    activeConnectionCount_(0),
    state_(ServerState::Created),
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

    // 创建并启动 IO 线程池，初始化 main loop 和 acceptor
    assert(loopThreadPool_ == nullptr);
    loopThreadPool_ = std::make_unique<EventLoopThreadPool>("TcpServerLoopPool", static_cast<int>(ioLoopNum_));
    loopThreadPool_->start();

    // 连接记录哈希表的外层以 EventLoop* 为键，start() 阶段一次性初始化完毕，运行期为纯只读结构，多线程并发查找（find）天然安全。
    const std::vector<EventLoop*> loops = loopThreadPool_->get_all_loops();
    assert(connectionRecordsByLoop_.empty());
    assert(!loops.empty());
    connectionRecordsByLoop_.reserve(loops.size());
    for (EventLoop* loop : loops) {
        assert(loop != nullptr);
        connectionRecordsByLoop_.emplace(loop, ConnectionRecords());
    }
    state_.store(ServerState::Running);

    // 在 main loop 所在线程创建 acceptor，监听 fd 的事件回调由 main loop 调度执行，保证线程安全。
    EventLoop& mainLoop = *loopThreadPool_->get_main_loop();
    InetAddress listenAddr(ip_, port_);
    acceptor_ = std::make_unique<Acceptor>(&mainLoop, listenAddr);
    acceptor_->set_connect_callback([this](Socket connSocket, const InetAddress& peerAddr) {
        on_connect(std::move(connSocket), peerAddr);
        });

    mainLoop.loop();

    // main loop 收到 quit() 请求退出后，先进入 Draining 状态，停止接受新连接，等待所有现有连接关闭完成后真正停止。
    shutdown_connections();
    acceptor_.reset();
    loopThreadPool_.reset();
    connectionRecordsByLoop_.clear();
    state_.store(ServerState::Stopped);
}

void TcpServer::stop() {
    ServerState expected = ServerState::Running;
    if (!state_.compare_exchange_strong(expected, ServerState::Draining)) {
        return;
    }

    EventLoop* mainLoop = nullptr;
    if (!loopThreadPool_) {
        spdlog::critical("TcpServer::stop() called but loopThreadPool_ is nullptr");
        return;
    }
    mainLoop = loopThreadPool_->get_main_loop();

    if (!mainLoop) {
        spdlog::critical("TcpServer::stop() called but mainLoop is nullptr");
        return;
    }
    mainLoop->quit();
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

    if (state_.load() != ServerState::Running) {
        // connSocket 仍持有刚 accept 到的 fd；提前返回会通过 Socket 析构关闭它。
        return;
    }

    EventLoop* ioLoop = loopThreadPool_->get_next_loop();
    assert(ioLoop != nullptr);
    assert(connectionRecordsByLoop_.find(ioLoop) != connectionRecordsByLoop_.end());

    // Socket 是 move-only 类型，用 shared_ptr 包装使 lambda 可拷贝以适配 std::function。
    auto connSocketPtr = std::make_shared<Socket>(std::move(connSocket));
    // 将新连接的 Socket 所有权转移到 ioLoop 线程，ioLoop 线程负责创建 TcpConnection 和 Channel，并管理其生命周期。
    ioLoop->run_in_loop([this, connSocketPtr, ioLoop, peerAddr, fd]() {
        const auto conn = create_connection(*ioLoop, std::move(*connSocketPtr), peerAddr);
        if (!conn) {
            return;
        }

        if (connectionCallback_) {
            connectionCallback_(conn);
            return;
        }

        spdlog::warn("TcpServer::on_connect(). connectionCallback is nullptr, fd: {}", fd);
        });
}

TcpConnectionPtr TcpServer::create_connection(EventLoop& ioLoop,
    Socket connSocket,
    const InetAddress& peerAddr) {
    assert(ioLoop.is_in_loop_thread());
    auto recordsIt = connectionRecordsByLoop_.find(&ioLoop);
    assert(recordsIt != connectionRecordsByLoop_.end());
    ConnectionRecords& localRecords = recordsIt->second;

    if (state_.load() != ServerState::Running) {
        // connSocket 还未移交给 TcpConnection；返回时 Socket 析构会关闭 fd。
        return nullptr;
    }

    const InetAddress localAddr = connSocket.local_address();
    auto conn = TcpConnection::create_connection(&ioLoop, std::move(connSocket), localAddr, peerAddr);

    // 配置 TCP 选项，开启 TCP_NODELAY 和 TCP keepalive。
    conn->set_tcp_no_delay(true);
    conn->set_keep_alive(true);

    // 配置 TcpConnection 回调，全部转发到 TcpServer 以统一管理。
    conn->set_message_callback([this](const TcpConnectionPtr& activeConn) {
        on_message(activeConn);
        });
    conn->set_close_callback([this](const TcpConnectionPtr& activeConn) {
        on_close(activeConn);
        });

    if (errorCallback_) {
        conn->set_error_callback([this](const TcpConnectionPtr& activeConn) {
            errorCallback_(activeConn);
            });
    }

    if (writeCompleteCallback_) {
        conn->set_write_complete_callback([this](const TcpConnectionPtr& activeConn) {
            writeCompleteCallback_(activeConn);
            });
    }

    if (highWaterMarkCallback_) {
        conn->set_high_water_mark_callback([this](const TcpConnectionPtr& activeConn) {
            highWaterMarkCallback_(activeConn, activeConn->get_write_buffer_size());
            }, highWaterMark_);
    }

    // 根据服务器级默认配置，为这条连接按需创建独立的空闲检测器。
    std::shared_ptr<ConnectionHeartbeat> heartbeat = create_connection_heartbeat(conn);

    if (state_.load() != ServerState::Running) {
        // conn 持有 Socket/Channel；丢弃 shared_ptr 会按 RAII 收口底层 fd。
        return nullptr;
    }

    // 连接和该连接的可选心跳策略一起注册和清理，真实连接表只由所属 loop 线程访问。
    localRecords[conn.get()] = ConnectionRecord{ conn, heartbeat };
    activeConnectionCount_.fetch_add(1);

    if (heartbeat) {
        heartbeat->start();
    }

    return conn;
}

std::shared_ptr<ConnectionHeartbeat> TcpServer::create_connection_heartbeat(const TcpConnectionPtr& conn) const {
    if (!connectionHeartbeatOptions_.enabled) {
        return nullptr;
    }

    // 策略对象只依赖 TcpConnection 的公共动作：refresh 通过 on_message 驱动，超时后调用 force_close。
    return std::make_shared<ConnectionHeartbeat>(conn,
        connectionHeartbeatOptions_.checkIntervalSeconds,
        connectionHeartbeatOptions_.idleTimeoutSeconds);
}

void TcpServer::on_message(const TcpConnectionPtr& conn) {
    // 任何成功上浮到 TcpServer 的读事件都视为连接仍然活跃，因此先刷新空闲窗口再继续协议分发。
    refresh_connection_heartbeat(conn);
    if (messageCallback_) {
        messageCallback_(conn);
        return;
    }

    spdlog::warn("TcpServer::on_message(). messageCallback is nullptr, fd: {}", conn ? conn->get_fd() : -1);
}

void TcpServer::on_close(const TcpConnectionPtr& conn) {
    remove_connection(conn);
    if (closeCallback_) {
        closeCallback_(conn);
        return;
    }

    spdlog::warn("TcpServer::on_close(). closeCallback is nullptr, fd: {}", conn ? conn->get_fd() : -1);
}

void TcpServer::remove_connection(const TcpConnectionPtr& conn) {
    assert(conn->get_loop()->is_in_loop_thread()); // 理论上应该总在 ioLoop 线程调用，assert 快速 Debug 代码可能的错误

    const int fd = conn->get_fd();
    auto recordsIt = connectionRecordsByLoop_.find(conn->get_loop());
    assert(recordsIt != connectionRecordsByLoop_.end());
    ConnectionRecords& localRecords = recordsIt->second;

    std::shared_ptr<ConnectionHeartbeat> heartbeat;
    auto it = localRecords.find(conn.get());
    if (it == localRecords.end()) {
        spdlog::error("TcpServer::remove_connection(). connection not found, fd={}", fd);
    }
    else {
        heartbeat = it->second.heartbeat;
        localRecords.erase(it);
        activeConnectionCount_.fetch_sub(1);
    }

    if (heartbeat) {
        heartbeat->stop();
    }
}

void TcpServer::refresh_connection_heartbeat(const TcpConnectionPtr& conn) {
    if (!connectionHeartbeatOptions_.enabled) {
        return;
    }

    assert(conn);
    assert(conn->get_loop()->is_in_loop_thread());
    auto recordsIt = connectionRecordsByLoop_.find(conn->get_loop());
    assert(recordsIt != connectionRecordsByLoop_.end());
    ConnectionRecords& localRecords = recordsIt->second;

    std::shared_ptr<ConnectionHeartbeat> heartbeat;
    auto it = localRecords.find(conn.get());
    if (it != localRecords.end()) {
        heartbeat = it->second.heartbeat;
    }

    if (heartbeat) {
        heartbeat->refresh();
    }
}

void TcpServer::shutdown_connections() {
    // 关闭所有连接：向每个 IO loop 投递 force_close，然后忙等待直到 activeConnectionCount_ 归零。
    // 调用时机：main loop 退出后，销毁线程池之前。

    state_.store(ServerState::Draining);
    for (auto& entry : connectionRecordsByLoop_) {
        EventLoop* loop = entry.first;
        ConnectionRecords& localRecords = entry.second;
        loop->run_in_loop([&localRecords]() {
            std::vector<TcpConnectionPtr> activeConnections;
            activeConnections.reserve(localRecords.size());
            for (auto& localEntry : localRecords) {
                if (localEntry.second.connection) {
                    activeConnections.push_back(localEntry.second.connection);
                }
            }

            for (const auto& conn : activeConnections) {
                conn->force_close();
            }

            activeConnections.clear();
            });
    }

    while (activeConnectionCount_.load() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
