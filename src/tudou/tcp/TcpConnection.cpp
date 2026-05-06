// ============================================================================
// TcpConnection.cpp
// TcpConnection 的实现保持单层事件编排：读、写、错、关和心跳各自只描述一层业务步骤。
// ============================================================================

#include "TcpConnection.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
    loop_->assert_in_loop_thread();

    const size_t oldLen = writeBuffer_->readable_bytes();
    writeBuffer_->write_to_buffer(msg);
    const size_t newLen = writeBuffer_->readable_bytes();

    // 背压在开启写事件前判断，保证回调看到的是跨阈值瞬间的真实积压量。
    if (highWaterMarkCallback_ && oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        notify_high_water_mark_callback();
    }

    channel_->enable_writing();
}

std::string TcpConnection::receive() {
    loop_->assert_in_loop_thread();
    return readBuffer_->read_from_buffer();
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
    tie_channel_to_owner();

    // 上层可能先配置心跳、后完成建连；这里统一补启，避免状态分叉。
    if (heartbeatEnabled_) {
        start_app_heartbeat_timer();
    }
}

void TcpConnection::set_tcp_no_delay(bool on) {
    const int fd = channel_->get_fd();
    const int kEnable = on ? 1 : 0;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::error("TcpConnection::set_tcp_no_delay() failed, errno={} ({})", errno, strerror(errno));
    }
}

void TcpConnection::set_tcp_keepalive(bool on) {
    const int fd = channel_->get_fd();
    const int kEnable = on ? 1 : 0;
    constexpr int kKeepIdleSec = 60;
    constexpr int kKeepIntvlSec = 10;
    constexpr int kKeepCnt = 3;

    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &kEnable, sizeof(kEnable)) < 0) {
        spdlog::warn("TcpConnection: failed to enable SO_KEEPALIVE on fd {}, errno: {}", fd, errno);
        return;
    }

    if (!on) {
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

    if (intervalSeconds <= 0.0 || timeoutSeconds <= 0.0) {
        spdlog::warn("TcpConnection::enable_app_heartbeat() invalid args, interval={}, timeout={}, fd={}",
            intervalSeconds, timeoutSeconds, get_fd());
        return;
    }

    heartbeatEnabled_ = true;
    heartbeatIntervalSeconds_ = intervalSeconds;
    heartbeatTimeoutSeconds_ = timeoutSeconds;
    heartbeatPingMessage_ = pingMessage;
    refresh_last_read_time();

    start_app_heartbeat_timer();
}

void TcpConnection::disable_app_heartbeat() {
    loop_->assert_in_loop_thread();

    heartbeatEnabled_ = false;
    stop_app_heartbeat_timer();
}

void TcpConnection::on_read(Channel& channel) {
    loop_->assert_in_loop_thread();

    int savedErrno = 0;
    const ssize_t n = read_from_channel(channel, &savedErrno);
    if (n > 0) {
        // 任意入站数据都视为连接存活，统一刷新活跃时间后再通知上层。
        refresh_last_read_time();
        notify_message_callback();
        return;
    }

    if (n == 0) {
        close_connection(channel);
        return;
    }

    if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
        return;
    }

    record_socket_error("read", savedErrno);
    notify_error_callback();
    close_connection(channel);
}

ssize_t TcpConnection::read_from_channel(const Channel& channel, int* savedErrno) {
    return readBuffer_->read_from_fd(channel.get_fd(), savedErrno);
}

void TcpConnection::refresh_last_read_time() {
    lastReadTime_ = std::chrono::steady_clock::now();
}

void TcpConnection::notify_message_callback() {
    assert(messageCallback_ != nullptr);
    messageCallback_(shared_from_this());
}

void TcpConnection::on_write(Channel& channel) {
    loop_->assert_in_loop_thread();

    if (!channel.is_writing()) {
        spdlog::error("TcpConnection::on_write() but channel is not writing.");
        return;
    }

    int savedErrno = 0;
    const ssize_t n = write_to_channel(channel, &savedErrno);
    if (n < 0) {
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            return;
        }

        record_socket_error("write", savedErrno);
        notify_error_callback();
        close_connection(channel);
        return;
    }

    if (writeBuffer_->readable_bytes() == 0) {
        channel.disable_writing();
        notify_write_complete_callback();
    }
}

ssize_t TcpConnection::write_to_channel(const Channel& channel, int* savedErrno) {
    return writeBuffer_->write_to_fd(channel.get_fd(), savedErrno);
}

void TcpConnection::notify_write_complete_callback() {
    if (!writeCompleteCallback_) {
        spdlog::warn("TcpConnection::notify_write_complete_callback(). writeCompleteCallback is nullptr, fd: {}", get_fd());
        return;
    }
    writeCompleteCallback_(shared_from_this());
}

void TcpConnection::on_close(Channel& channel) {
    loop_->assert_in_loop_thread();
    close_connection(channel);
}

void TcpConnection::close_connection(Channel& channel) {
    if (isClosed_) {
        return;
    }

    isClosed_ = true;

    // 所有关闭路径都统一先停心跳，再注销事件关注，最后通知上层回收连接。
    stop_app_heartbeat_timer();
    channel.disable_all();
    notify_close_callback();
}

void TcpConnection::notify_close_callback() {
    assert(closeCallback_ != nullptr);

    // 关闭回调可能导致连接对象被移出容器，这里显式保活到回调结束。
    std::shared_ptr<TcpConnection> guardThis{ shared_from_this() };
    closeCallback_(guardThis);
}

void TcpConnection::on_error(Channel& channel) {
    loop_->assert_in_loop_thread();
    notify_error_callback();
    close_connection(channel);
}

void TcpConnection::record_socket_error(const char* action, int errorCode) {
    lastErrorCode_ = errorCode;
    lastErrorMsg_ = std::string(action) + " error: " + std::to_string(errorCode);
    spdlog::error("TcpConnection::{}() failed, errno={} ({})", action, errorCode, strerror(errorCode));
}

void TcpConnection::notify_error_callback() {
    if (!errorCallback_) {
        spdlog::warn("TcpConnection::notify_error_callback(). errorCallback is nullptr, fd: {}", get_fd());
        return;
    }
    errorCallback_(shared_from_this());
}

void TcpConnection::notify_high_water_mark_callback() {
    if (!highWaterMarkCallback_) {
        spdlog::warn("TcpConnection::notify_high_water_mark_callback(). highWaterMarkCallback is nullptr, fd: {}", get_fd());
        return;
    }
    highWaterMarkCallback_(shared_from_this());
}

void TcpConnection::tie_channel_to_owner() {
    auto ptr = shared_from_this();
    channel_->tie_to_object(ptr);
}

void TcpConnection::start_app_heartbeat_timer() {
    loop_->assert_in_loop_thread();

    stop_app_heartbeat_timer();
    if (!heartbeatEnabled_ || isClosed_) {
        return;
    }

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

    const auto now = std::chrono::steady_clock::now();
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReadTime_).count();
    const auto timeoutMs = static_cast<long long>(heartbeatTimeoutSeconds_ * 1000.0);

    if (is_heartbeat_timeout(now)) {
        spdlog::warn("TcpConnection heartbeat timeout, fd={}, idleMs={}, timeoutMs={}", get_fd(), idleMs, timeoutMs);
        close_connection(*channel_);
        return;
    }

    if (!heartbeatPingMessage_.empty()) {
        // 心跳探测复用统一 send 路径，确保背压与写完成语义完全一致。
        send(heartbeatPingMessage_);
    }
}

bool TcpConnection::is_heartbeat_timeout(std::chrono::steady_clock::time_point now) const {
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReadTime_).count();
    const auto timeoutMs = static_cast<long long>(heartbeatTimeoutSeconds_ * 1000.0);
    return idleMs >= timeoutMs;
}
