// ============================================================================
// TcpConnection.cpp
// TcpConnection 的实现：Socket 接管 fd 所有权，连接只关心会话语义。
// ============================================================================

#include "TcpConnection.h"

#include <cassert>
#include <cerrno>
#include <cstring>

#include "spdlog/spdlog.h"

#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"

std::shared_ptr<TcpConnection> TcpConnection::create(EventLoop* loop,
    Socket connSocket,
    const InetAddress& localAddr,
    const InetAddress& peerAddr) {
    std::shared_ptr<TcpConnection> conn(new TcpConnection(loop, std::move(connSocket), localAddr, peerAddr));
    conn->activate();
    return conn;
}

TcpConnection::TcpConnection(EventLoop* loop, Socket connSocket, const InetAddress& localAddr, const InetAddress& peerAddr) :
    loop_(loop),
    connSocket_(std::move(connSocket)),
    channel_(std::make_unique<Channel>(loop, connSocket_.fd())),
    localAddr_(localAddr),
    peerAddr_(peerAddr),
    readBuffer_(std::make_unique<Buffer>()),
    writeBuffer_(std::make_unique<Buffer>()),
    highWaterMark_(64 * 1024 * 1024),
    messageCallback_(nullptr),
    closeCallback_(nullptr),
    errorCallback_(nullptr),
    writeCompleteCallback_(nullptr),
    highWaterMarkCallback_(nullptr),
    isClosed_(false) {
    channel_->set_read_callback([this](Channel& ch) { on_read(ch); });
    channel_->set_write_callback([this](Channel& ch) { on_write(ch); });
    channel_->set_close_callback([this](Channel& ch) { on_close(ch); });
    channel_->set_error_callback([this](Channel& ch) { on_error(ch); });
}

TcpConnection::~TcpConnection() {
    spdlog::debug("TcpConnection::~TcpConnection() called. fd: {}", connSocket_.fd());
}

void TcpConnection::activate() {
    assert(loop_->is_in_loop_thread());
    channel_->tie_to_object(shared_from_this());
    channel_->enable_reading();
}

void TcpConnection::send(const std::string& msg) {
    if (!loop_->is_in_loop_thread()) {
        std::shared_ptr<TcpConnection> self = shared_from_this();
        loop_->queue_in_loop([self, msg]() {
            self->send_in_loop(msg);
            });
        return;
    }

    send_in_loop(msg);
}

void TcpConnection::send_in_loop(const std::string& msg) {
    assert(loop_->is_in_loop_thread());
    if (isClosed_) {
        return;
    }

    const size_t oldLen = writeBuffer_->readable_bytes();
    writeBuffer_->write_to_buffer(msg);
    const size_t newLen = writeBuffer_->readable_bytes();

    // 只有当高水位回调存在且刚好从未越过高水位变为越过高水位时才触发回调，避免重复触发。
    if (highWaterMarkCallback_ && oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        handle_high_water_mark_callback();
    }

    channel_->enable_writing();
}

std::string TcpConnection::receive() {
    assert(loop_->is_in_loop_thread());
    return readBuffer_->read_from_buffer();
}

void TcpConnection::set_tcp_no_delay(bool on) {
    connSocket_.set_tcp_no_delay(on);
}

void TcpConnection::set_keep_alive(bool on) {
    connSocket_.set_keep_alive(on);
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

void TcpConnection::force_close() {
    if (!loop_->is_in_loop_thread()) {
        std::shared_ptr<TcpConnection> self = shared_from_this();
        loop_->queue_in_loop([self]() {
            self->force_close_in_loop();
            });
        return;
    }

    force_close_in_loop();
}

void TcpConnection::force_close_in_loop() {
    assert(loop_->is_in_loop_thread());
    close_connection(*channel_);
}

void TcpConnection::on_read(Channel& channel) {
    assert(loop_->is_in_loop_thread());

    int savedErrno = 0;
    const ssize_t n = readBuffer_->read_from_fd(channel.get_fd(), &savedErrno);
    if (n > 0) {
        handle_message_callback();
        return;
    }

    if (n == 0) {
        close_connection(channel);
        return;
    }

    if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
        return;
    }

    spdlog::error("TcpConnection::on_read() failed, errno={} ({})", savedErrno, strerror(savedErrno));
    handle_error_callback();
    close_connection(channel);
}

void TcpConnection::handle_message_callback() {
    assert(messageCallback_ != nullptr);
    messageCallback_(shared_from_this());
}

void TcpConnection::on_write(Channel& channel) {
    assert(loop_->is_in_loop_thread());

    int savedErrno = 0;
    const ssize_t n = writeBuffer_->write_to_fd(channel.get_fd(), &savedErrno);
    if (n < 0) {
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            return;
        }

        spdlog::error("TcpConnection::on_write() failed, errno={} ({})", savedErrno, strerror(savedErrno));
        handle_error_callback();
        close_connection(channel);
        return;
    }

    if (writeBuffer_->readable_bytes() == 0) {
        channel.disable_writing();
        handle_write_complete_callback();
    }
}

void TcpConnection::handle_write_complete_callback() {
    if (!writeCompleteCallback_) {
        spdlog::warn("TcpConnection::handle_write_complete_callback(). writeCompleteCallback is nullptr, fd: {}", get_fd());
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
    channel.disable_all();
    handle_close_callback(); // TcpServer 删除连接析构 TcpConnection 对象，TcpConnection 自动管理 Socket、Channel 等资源的生命周期，保证资源正确释放。特别是 Channel 的析构会自动从 Poller 注销，避免悬挂事件。
}

void TcpConnection::handle_close_callback() {
    assert(closeCallback_ != nullptr);

    std::shared_ptr<TcpConnection> guardThis{ shared_from_this() };
    closeCallback_(guardThis);
}

void TcpConnection::on_error(Channel& channel) {
    assert(loop_->is_in_loop_thread());
    handle_error_callback();
    close_connection(channel);
}

void TcpConnection::handle_error_callback() {
    if (!errorCallback_) {
        spdlog::warn("TcpConnection::handle_error_callback(). errorCallback is nullptr, fd: {}", get_fd());
        return;
    }

    errorCallback_(shared_from_this());
}

void TcpConnection::handle_high_water_mark_callback() {
    if (!highWaterMarkCallback_) {
        spdlog::warn("TcpConnection::handle_high_water_mark_callback(). highWaterMarkCallback is nullptr, fd: {}", get_fd());
        return;
    }

    highWaterMarkCallback_(shared_from_this());
}


