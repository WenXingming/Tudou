/**
 * @file TcpConnection.cpp
 * @brief 面向连接的 TCP 会话封装，负责收发缓冲、事件回调与状态管理。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "TcpConnection.h"

#include <assert.h>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <netinet/in.h>

#include "spdlog/spdlog.h"
#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"

TcpConnection::TcpConnection(EventLoop* loop, int connFd, const InetAddress& localAddr, const InetAddress& peerAddr) :
    loop_(loop),
    channel_(std::make_unique<Channel>(loop, connFd)),
    localAddr_(localAddr),
    peerAddr_(peerAddr),
    highWaterMark_(64 * 1024 * 1024), // 64 MB
    readBuffer_(std::make_unique<Buffer>()),
    writeBuffer_(std::make_unique<Buffer>()), // Don't forget! Or cause segfault!
    messageCallback_(nullptr),
    closeCallback_(nullptr),
    errorCallback_(nullptr),
    writeCompleteCallback_(nullptr),
    highWaterMarkCallback_(nullptr),
    lastErrorCode_(0),
    lastErrorMsg_(""),
    isClosed_(false),
    heartbeatEnabled_(false),
    heartbeatIntervalSeconds_(0.0),
    heartbeatTimeoutSeconds_(0.0),
    heartbeatPingMessage_("PING\r\n"),
    heartbeatTimerId_(),
    lastReadTime_(std::chrono::steady_clock::now()) {

    channel_->set_read_callback([this](Channel& ch) { on_read(ch); });
    channel_->set_write_callback([this](Channel& ch) { on_write(ch); });
    channel_->set_close_callback([this](Channel& ch) { on_close(ch); });
    channel_->set_error_callback([this](Channel& ch) { on_error(ch); });
    channel_->enable_reading();
}

TcpConnection::~TcpConnection() {
    spdlog::debug("TcpConnection::~TcpConnection() called. fd: {}", channel_->get_fd());
}

void TcpConnection::send(const std::string& msg) {
    // TODO: 考虑增加使用 run_in_loop 提供跨线程调用
    loop_->assert_in_loop_thread();

    size_t oldLen = writeBuffer_->readable_bytes();
    writeBuffer_->write_to_buffer(msg);
    size_t newLen = writeBuffer_->readable_bytes();

    // 检查是否超过高水位
    if (highWaterMarkCallback_ && oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        handle_high_water_mark_callback();
    }

    channel_->enable_writing();
}

std::string TcpConnection::receive() {
    loop_->assert_in_loop_thread();
    return readBuffer_->read_from_buffer(); // 不能用 run_in_loop，局部变量会在函数返回后失效
}

void TcpConnection::set_message_callback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpConnection::set_close_callback(CloseCallback cb) {
    closeCallback_ = std::move(cb);
}

void TcpConnection::set_error_callback(ErrorCallback cb) {
    errorCallback_ = std::move(cb);
}

void TcpConnection::set_write_complete_callback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpConnection::set_high_water_mark_callback(HighWaterMarkCallback cb, size_t highWaterMark) {
    highWaterMarkCallback_ = std::move(cb);
    highWaterMark_ = highWaterMark;
}

void TcpConnection::connection_establish() {
    auto ptr = shared_from_this();
    channel_->tie_to_object(ptr);

    // 若在 connection_establish 前已启用心跳（上层先配置再建连），这里补启定时器
    if (heartbeatEnabled_) {
        start_app_heartbeat_timer();
    }
}

void TcpConnection::set_tcp_no_delay(bool on) {
    int fd = channel_->get_fd();
    int kEnable = on ? 1 : 0;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::error("TcpConnection::set_tcp_no_delay() failed, errno={} ({})", errno, strerror(errno));
    }
}

void TcpConnection::set_tcp_keepalive(bool on) {
    // TODO: 这些参数可以通过 Acceptor 的构造函数、TCPServer 的构造函数或者配置文件进行配置，目前先写死
    int fd = channel_->get_fd();
    int kEnable = on ? 1 : 0;
    constexpr int kKeepIdleSec = 60;
    constexpr int kKeepIntvlSec = 10;
    constexpr int kKeepCnt = 3;

    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("TcpConnection: failed to enable SO_KEEPALIVE on fd {}, errno: {}", fd, errno);
        return;
    }

    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &kKeepIdleSec, sizeof(kKeepIdleSec)) < 0) {
        spdlog::warn("TcpConnection: failed to set TCP_KEEPIDLE on fd {}, errno: {}", fd, errno);
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &kKeepIntvlSec, sizeof(kKeepIntvlSec)) < 0) {
        spdlog::warn("TcpConnection: failed to set TCP_KEEPINTVL on fd {}, errno: {}", fd, errno);
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &kKeepCnt, sizeof(kKeepCnt)) < 0) {
        spdlog::warn("TcpConnection: failed to set TCP_KEEPCNT on fd {}, errno: {}", fd, errno);
    }
}

void TcpConnection::enable_app_heartbeat(double intervalSeconds, double timeoutSeconds, const std::string& pingMessage) {
    loop_->assert_in_loop_thread();

    // 最小约束：周期与超时时间必须为正，避免无意义或异常配置
    if (intervalSeconds <= 0.0 || timeoutSeconds <= 0.0) {
        spdlog::warn("TcpConnection::enable_app_heartbeat() invalid args, interval={}, timeout={}, fd={}",
            intervalSeconds, timeoutSeconds, get_fd());
        return;
    }

    heartbeatEnabled_ = true;
    heartbeatIntervalSeconds_ = intervalSeconds;
    heartbeatTimeoutSeconds_ = timeoutSeconds;
    heartbeatPingMessage_ = pingMessage;
    // 启用时刷新一次活跃时间，避免刚开启就被立即判定超时
    lastReadTime_ = std::chrono::steady_clock::now();

    start_app_heartbeat_timer();
}

void TcpConnection::disable_app_heartbeat() {
    loop_->assert_in_loop_thread();

    heartbeatEnabled_ = false;
    stop_app_heartbeat_timer();
}

void TcpConnection::on_read(Channel& channel) {
    loop_->assert_in_loop_thread();

    // 从 fd 读数据到 readBuffer，然后触发上层回调处理数据
    int fd = channel.get_fd();
    int savedErrno = 0;
    ssize_t n = readBuffer_->read_from_fd(fd, &savedErrno);
    if (n > 0) {
        // 任意入站数据都代表连接仍活跃，用于心跳超时判断
        lastReadTime_ = std::chrono::steady_clock::now();
        handle_message_callback();
        return;
    }
    // 对端关闭
    if (n == 0) {
        on_close(channel);
        return;
    }
    // 发生错误
    // 本轮数据读完，等下次 EPOLLIN 事件再读
    if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
        return;
    }
    lastErrorCode_ = savedErrno;
    lastErrorMsg_ = "read error: " + std::to_string(savedErrno);
    spdlog::error("TcpConnection::on_read(). read error: {}", savedErrno);
    on_error(channel);
}

void TcpConnection::on_write(Channel& channel) {
    loop_->assert_in_loop_thread();

    if (!channel.is_writing()) {
        spdlog::error("TcpConnection::on_write() but channel is not writing.");
        return;
    }

    int savedErrno = 0;
    int fd = channel.get_fd();
    ssize_t n = writeBuffer_->write_to_fd(fd, &savedErrno);

    if (n == -1) {
        // 本轮写完，等下次 EPOLLOUT 事件再写
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            return;
        }
        lastErrorCode_ = savedErrno;
        lastErrorMsg_ = "write error: " + std::to_string(savedErrno);
        spdlog::error("TcpConnection::on_write(). write error: {}", savedErrno);
        on_error(channel);
    }

    if (writeBuffer_->readable_bytes() == 0) {
        channel.disable_writing();
        handle_write_complete_callback();
    }
}

void TcpConnection::on_close(Channel& channel) {
    loop_->assert_in_loop_thread();

    // 防止多路径（EOF/错误/心跳超时）重复进入关闭流程
    if (isClosed_) {
        return;
    }
    isClosed_ = true;

    // 连接关闭时取消心跳定时器，避免后续 tick 访问已关闭连接
    stop_app_heartbeat_timer();

    channel.disable_all();
    handle_close_callback();
}

void TcpConnection::on_error(Channel& channel) {
    loop_->assert_in_loop_thread();

    handle_error_callback();
    on_close(channel);
}

void TcpConnection::handle_message_callback() {
    assert(messageCallback_ != nullptr);
    messageCallback_(shared_from_this());
}

void TcpConnection::handle_close_callback() {
    assert(closeCallback_ != nullptr);

    std::shared_ptr<TcpConnection> guardThis{ shared_from_this() }; // 防止回调过程中对象被析构，延长生命周期
    closeCallback_(guardThis);
}

void TcpConnection::handle_error_callback() {
    if (!errorCallback_) {
        spdlog::warn("TcpConnection::handle_error_callback(). errorCallback is nullptr, fd: {}", get_fd());
        return;
    }
    errorCallback_(shared_from_this());
}

void TcpConnection::handle_write_complete_callback() {
    if (!writeCompleteCallback_) {
        spdlog::warn("TcpConnection::handle_write_complete_callback(). writeCompleteCallback is nullptr, fd: {}", get_fd());
        return;
    }
    writeCompleteCallback_(shared_from_this());
}

void TcpConnection::handle_high_water_mark_callback() {
    if (!highWaterMarkCallback_) {
        spdlog::warn("TcpConnection::handle_high_water_mark_callback(). highWaterMarkCallback is nullptr, fd: {}", get_fd());
        return;
    }
    highWaterMarkCallback_(shared_from_this());
}

void TcpConnection::start_app_heartbeat_timer() {
    loop_->assert_in_loop_thread();

    // 统一先停后启，保证同一连接只存在一个心跳定时器
    stop_app_heartbeat_timer();
    if (!heartbeatEnabled_ || isClosed_) {
        return;
    }

    // 定时器回调通过 weak_ptr 提升安全性：连接已析构时不再执行逻辑
    std::weak_ptr<TcpConnection> weakConn = shared_from_this();
    heartbeatTimerId_ = loop_->run_every(heartbeatIntervalSeconds_, [weakConn]() {
        auto conn = weakConn.lock();
        if (!conn) {
            return;
        }
        conn->on_heartbeat_tick();
        });
}

void TcpConnection::stop_app_heartbeat_timer() {
    loop_->assert_in_loop_thread();

    if (!heartbeatTimerId_.valid()) {
        return;
    }
    loop_->cancel(heartbeatTimerId_);
    heartbeatTimerId_ = TimerId();
}

void TcpConnection::on_heartbeat_tick() {
    loop_->assert_in_loop_thread();

    if (!heartbeatEnabled_ || isClosed_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReadTime_).count();
    const auto timeoutMs = static_cast<long long>(heartbeatTimeoutSeconds_ * 1000.0);

    // 超时策略：超过阈值仍未收到任何数据，判定连接失活并主动关闭
    if (idleMs >= timeoutMs) {
        spdlog::warn("TcpConnection heartbeat timeout, fd={}, idleMs={}, timeoutMs={}", get_fd(), idleMs, timeoutMs);
        on_close(*channel_);
        return;
    }

    // 非超时则发送心跳探测包（若配置为空字符串则只做被动超时检测）
    if (!heartbeatPingMessage_.empty()) {
        send(heartbeatPingMessage_);
    }
}
