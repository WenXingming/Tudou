// ============================================================================
// TcpConnection.cpp
// TcpConnection 的实现：Socket 接管 fd 所有权，连接只关心会话语义。
// ============================================================================

#include "TcpConnection.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <unistd.h>

#include "spdlog/spdlog.h"
#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"

TcpConnection::TcpConnection(EventLoop* loop, Socket connSocket, const InetAddress& localAddr, const InetAddress& peerAddr) :
    loop_(loop),
    connSocket_(std::move(connSocket)),
    channel_(std::make_unique<Channel>(loop, connSocket_.fd())),
    localAddr_(localAddr),
    peerAddr_(peerAddr),
    highWaterMark_(64 * 1024 * 1024),
    readBuffer_(std::make_unique<Buffer>()),
    writeBuffer_(std::make_unique<Buffer>()),
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
    spdlog::debug("TcpConnection::~TcpConnection() called. fd: {}", connSocket_.fd());
}

void TcpConnection::send(const std::string& msg) {
    assert(loop_->is_in_loop_thread());

    const size_t oldLen = writeBuffer_->readable_bytes();
    writeBuffer_->write_to_buffer(msg);
    const size_t newLen = writeBuffer_->readable_bytes();

    if (highWaterMarkCallback_ && oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        notify_high_water_mark_callback();
    }

    channel_->enable_writing();
}

std::string TcpConnection::receive() {
    assert(loop_->is_in_loop_thread());
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

    if (heartbeatEnabled_) {
        start_app_heartbeat_timer();
    }
}

void TcpConnection::enable_app_heartbeat(double intervalSeconds, double timeoutSeconds, const std::string& pingMessage) {
    assert(loop_->is_in_loop_thread());

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
    assert(loop_->is_in_loop_thread());

    heartbeatEnabled_ = false;
    stop_app_heartbeat_timer();
}

void TcpConnection::on_read(Channel& channel) {
    assert(loop_->is_in_loop_thread());

    int savedErrno = 0;
    const ssize_t n = read_from_channel(channel, &savedErrno);
    if (n > 0) {
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
    assert(loop_->is_in_loop_thread());

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
    assert(loop_->is_in_loop_thread());
    close_connection(channel);
}

void TcpConnection::close_connection(Channel& channel) {
    if (isClosed_) {
        return;
    }

    isClosed_ = true;
    stop_app_heartbeat_timer();
    channel.disable_all();
    notify_close_callback();
}

void TcpConnection::notify_close_callback() {
    assert(closeCallback_ != nullptr);

    std::shared_ptr<TcpConnection> guardThis{ shared_from_this() };
    closeCallback_(guardThis);
}

void TcpConnection::on_error(Channel& channel) {
    assert(loop_->is_in_loop_thread());
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
    assert(loop_->is_in_loop_thread());

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
    assert(loop_->is_in_loop_thread());

    if (!heartbeatTimerId_.valid()) {
        return;
    }

    loop_->cancel(heartbeatTimerId_);
    heartbeatTimerId_ = TimerId();
}

void TcpConnection::on_heartbeat_tick() {
    assert(loop_->is_in_loop_thread());

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
        send(heartbeatPingMessage_);
    }
}

bool TcpConnection::is_heartbeat_timeout(std::chrono::steady_clock::time_point now) const {
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReadTime_).count();
    const auto timeoutMs = static_cast<long long>(heartbeatTimeoutSeconds_ * 1000.0);
    return idleMs >= timeoutMs;
}
