// ============================================================================
// TcpServer 负责监听端口、分配连接到 IO 线程，并转发 TcpConnection 事件。
// 停止时先停止接收，再由各 IO 线程收口现有连接。
// ============================================================================

#include "tudou/tcp/TcpServer.h"

#include <cassert>
#include <vector>

#include "tudou/tcp/InetAddress.h"
#include "spdlog/spdlog.h"
#include "tudou/tcp/Acceptor.h"
#include "tudou/reactor/EventLoop.h"
#include "tudou/tcp/TcpConnection.h"
#include "tudou/reactor/EventLoopThread.h"
#include "tudou/reactor/EventLoopThreadPool.h"

namespace {

constexpr size_t kDefaultHighWaterMark = 64 * 1024 * 1024; // 64 MB

} // namespace

TcpServer::TcpServer(std::string ip, uint16_t port, size_t ioLoopNum) :
    ip_(std::move(ip)),
    port_(port),
    ioLoopNum_(ioLoopNum),
    loopThreadPool_(nullptr),
    acceptor_(nullptr),
    accepting_(false),
    connectionRecordsByLoop_(),
    heartbeatCheckIntervalSeconds_(0.0),
    heartbeatIdleTimeoutSeconds_(0.0),
    activeConnectionCount_(0),
    shutdownMutex_(),
    shutdownCondition_(),
    connectionCallback_(nullptr),
    messageCallback_(nullptr),
    closeCallback_(nullptr),
    errorCallback_(nullptr),
    writeCompleteCallback_(nullptr),
    highWaterMark_(kDefaultHighWaterMark),
    highWaterMarkCallback_(nullptr) {
}

TcpServer::~TcpServer() = default;

void TcpServer::start() {
    spdlog::debug("TcpServer::start() called, starting server at {}:{}", ip_, port_);

    // 创建并启动 IO 线程池，初始化 main loop 和 acceptor
    assert(loopThreadPool_ == nullptr);
    loopThreadPool_ = std::make_unique<EventLoopThreadPool>(
        "TcpServerLoopPool",
        static_cast<int>(ioLoopNum_),
        EventLoopThreadPool::ThreadInitCallback()
    );
    loopThreadPool_->start();

    // 连接表按所属 EventLoop 分片，start() 阶段一次性创建各分片。
    const std::vector<EventLoop*> loops = loopThreadPool_->get_all_loops();
    assert(connectionRecordsByLoop_.empty());
    assert(!loops.empty());
    connectionRecordsByLoop_.reserve(loops.size());
    for (EventLoop* loop : loops) {
        assert(loop != nullptr);
        connectionRecordsByLoop_.emplace(loop, std::unordered_map<TcpConnection*, TcpConnectionPtr>());
    }
    accepting_.store(true);

    // 在 main loop 所在线程创建 acceptor，监听 fd 的事件回调由 main loop 调度执行，保证线程安全。
    EventLoop& mainLoop = *loopThreadPool_->get_main_loop();
    InetAddress listenAddr(ip_, port_);
    acceptor_ = std::make_unique<Acceptor>(&mainLoop, listenAddr);
    acceptor_->set_connect_callback([this](Socket connSocket, const InetAddress& peerAddr) {
        on_connect(std::move(connSocket), peerAddr);
        });

    mainLoop.loop();

    // main loop 退出后，等待所有现有连接完成收口。
    accepting_.store(false);
    shutdown_connections();
    acceptor_.reset();
    loopThreadPool_.reset();
    connectionRecordsByLoop_.clear();
}

void TcpServer::stop() {
    bool expected = true;
    if (!accepting_.compare_exchange_strong(expected, false)) {
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
        heartbeatCheckIntervalSeconds_ = 0.0;
        heartbeatIdleTimeoutSeconds_ = 0.0;
        return;
    }

    heartbeatCheckIntervalSeconds_ = checkIntervalSeconds;
    heartbeatIdleTimeoutSeconds_ = idleTimeoutSeconds;
}

void TcpServer::on_connect(Socket connSocket, const InetAddress& peerAddr) {
    EventLoop* mainLoop = loopThreadPool_->get_main_loop();
    assert(mainLoop != nullptr);
    assert(mainLoop->is_in_loop_thread());

    const int fd = connSocket.fd();
    spdlog::info("TcpServer: New connection from {} on fd {}", peerAddr.get_ip_port(), fd);

    if (!accepting_.load()) {
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
    auto& localRecords = recordsIt->second;

    if (!accepting_.load()) {
        // connSocket 还未移交给 TcpConnection；返回时 Socket 析构会关闭 fd。
        return nullptr;
    }

    // 接入策略由 TcpServer 决定，再将已配置的 Socket 交给连接管理。
    connSocket.set_tcp_no_delay(true);
    connSocket.set_keep_alive(true);
    auto conn = TcpConnection::create_connection(&ioLoop,
        std::move(connSocket),
        peerAddr,
        heartbeatCheckIntervalSeconds_,
        heartbeatIdleTimeoutSeconds_);

    // 配置 TcpConnection 回调，TcpServer 把 6 种 callback 从用户设置转发到每个 TcpConnection
    conn->set_message_callback([this](const TcpConnectionPtr& activeConn) {
        on_message(activeConn);
        });
    conn->set_close_callback([this](const TcpConnectionPtr& activeConn) {
        on_close(activeConn);
        });

    if (errorCallback_) {
        conn->set_error_callback(errorCallback_);
    }

    if (writeCompleteCallback_) {
        conn->set_write_complete_callback(writeCompleteCallback_);
    }

    if (highWaterMarkCallback_) {
        conn->set_high_water_mark_callback([this](const TcpConnectionPtr& activeConn) {
            highWaterMarkCallback_(activeConn, activeConn->get_write_buffer_size());
            }, highWaterMark_);
    }

    if (!accepting_.load()) {
        // conn 持有 Socket/Channel；丢弃 shared_ptr 会按 RAII 收口底层 fd。
        return nullptr;
    }

    // 连接表只保存连接本身；连接的附属资源由 TcpConnection 自己管理。
    localRecords[conn.get()] = conn;
    activeConnectionCount_.fetch_add(1);

    return conn;
}

void TcpServer::on_message(const TcpConnectionPtr& conn) {
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
    auto& localRecords = recordsIt->second;

    auto it = localRecords.find(conn.get());
    if (it == localRecords.end()) {
        spdlog::error("TcpServer::remove_connection(). connection not found, fd={}", fd);
    }
    else {
        localRecords.erase(it);
        activeConnectionCount_.fetch_sub(1);
    }
}

void TcpServer::shutdown_connections() {
    // 每个 IO loop 摘出本线程的连接记录并主动关闭连接，最后等待全部记录收口。
    // 连接自身负责停止心跳，清空 close 回调则避免关闭时重复访问服务器连接表。

    for (auto& entry : connectionRecordsByLoop_) {
        EventLoop* loop = entry.first;
        auto& localRecords = entry.second;
        loop->run_in_loop([this, &localRecords]() {
            std::vector<TcpConnectionPtr> pending;
            pending.reserve(localRecords.size());
            for (auto it = localRecords.begin(); it != localRecords.end(); it = localRecords.erase(it)) {
                activeConnectionCount_.fetch_sub(1);
                    pending.push_back(std::move(it->second));
            }
            shutdownCondition_.notify_one();

            for (auto& connection : pending) {
                connection->set_close_callback(nullptr);
                connection->set_message_callback(nullptr);
                connection->force_close();
            }
            });
    }

    std::unique_lock<std::mutex> lock(shutdownMutex_);
    shutdownCondition_.wait(lock, [this]() {
        return activeConnectionCount_.load() == 0;
        });
}
