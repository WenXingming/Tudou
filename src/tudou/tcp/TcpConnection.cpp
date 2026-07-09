// ============================================================================
// TcpConnection.cpp
// TcpConnection 的实现：Socket 接管 fd 所有权，连接只关心会话语义。
// ============================================================================

#include "tudou/tcp/TcpConnection.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "spdlog/spdlog.h"

#include "base/ScopedFd.h"
#include "tudou/tcp/Buffer.h"
#include "tudou/reactor/Channel.h"
#include "tudou/reactor/EventLoop.h"

namespace {

bool is_regular_file(int fd) {
    struct stat st;
    return ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

std::shared_ptr<TcpConnection> TcpConnection::create_connection(EventLoop* loop, Socket connSocket, const InetAddress& localAddr, const InetAddress& peerAddr) {
    std::shared_ptr<TcpConnection> conn(new TcpConnection(loop, std::move(connSocket), localAddr, peerAddr));
    conn->channel_->tie_to_object(conn);
    conn->channel_->enable_reading(); // 避免还未创建好触发 epoll 和回调
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
    pendingFile_(),
    hasPendingFile_(false),
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

// 线程屏障。与用户业务代码交互，用户可能会在业务线程池中调用 send()
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

void TcpConnection::send(std::string&& msg) {
    if (!loop_->is_in_loop_thread()) {
        std::shared_ptr<TcpConnection> self = shared_from_this();
        loop_->queue_in_loop([self, msg = std::move(msg)]() {
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

    if (has_pending_file()) {
        spdlog::error("TcpConnection::send_in_loop() cannot preserve order while file send is pending");
        handle_error_callback();
        close_connection(*channel_);
        return;
    }

    size_t writtenLen = 0;
    const size_t oldLen = writeBuffer_->readable_bytes();

    // 场景 1：当前无积压，直接 write 写入新数据
    if (oldLen == 0 && !channel_->is_writing()) {
        const ssize_t n = ::write(connSocket_.fd(), msg.data(), msg.size());

        // 非瞬态写错误：记录日志并关闭连接
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            spdlog::error("TcpConnection::send_in_loop() failed, errno={} ({})", errno, strerror(errno));
            handle_error_callback();
            close_connection(*channel_);
            return;
        }

        if (n >= 0) {
            writtenLen = static_cast<size_t>(n);
        }

        // 判断是否完整写入。是：触发回调后直接返回
        if (writtenLen == msg.size()) {
            handle_write_complete_callback();
            return;
        }
    }
    // 场景 2：当前发送缓冲中已有积压，使用 writev 合并发送积压缓冲和新 msg，省去在用户态拷贝拼接的开销
    else if (oldLen > 0) {
        struct iovec iov[2];
        iov[0].iov_base = const_cast<char*>(writeBuffer_->readable_start_ptr());
        iov[0].iov_len = oldLen;
        iov[1].iov_base = const_cast<char*>(msg.data());
        iov[1].iov_len = msg.size();

        const ssize_t n = ::writev(connSocket_.fd(), iov, 2);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            spdlog::error("TcpConnection::send_in_loop() writev failed, errno={} ({})", errno, strerror(errno));
            handle_error_callback();
            close_connection(*channel_);
            return;
        }

        if (n >= 0) {
            size_t bytesWritten = static_cast<size_t>(n);
            if (bytesWritten >= oldLen) {
                // 积压缓冲区数据全部成功发送
                writeBuffer_->advance_read_index(oldLen); // 清空旧缓冲
                size_t msgBytesWritten = bytesWritten - oldLen;
                writtenLen = msgBytesWritten;

                if (writtenLen == msg.size()) {
                    // 新旧数据全发完
                    channel_->disable_writing();
                    handle_write_complete_callback();
                    return;
                }
            } else {
                // 积压缓冲区仅部分发送，新 msg 完全没发
                writeBuffer_->advance_read_index(bytesWritten); // 仅消费掉已发出的旧数据
                writtenLen = 0;
            }
        }
    }

    // 将未发出数据追加到应用层发送缓冲，注册写事件驱动 Reactor 后续发送
    if (writtenLen < msg.size()) {
        writeBuffer_->write_to_buffer(msg.data() + writtenLen, msg.size() - writtenLen);
    }
    channel_->enable_writing();

    // 只有当高水位回调存在且刚好从未越过高水位变为越过高水位时才触发回调，避免重复触发。
    const size_t newLen = writeBuffer_->readable_bytes();
    if (highWaterMarkCallback_ && oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        handle_high_water_mark_callback();
    }
}

void TcpConnection::send_file(std::shared_ptr<ScopedFd> file, size_t size, size_t offset) {
    if (!loop_->is_in_loop_thread()) {
        std::shared_ptr<TcpConnection> self = shared_from_this();
        loop_->queue_in_loop([self, file = std::move(file), size, offset]() {
            self->send_file_in_loop(file, size, offset);
            });
        return;
    }

    send_file_in_loop(std::move(file), size, offset);
}

void TcpConnection::send_file_with_header(const std::string& header, std::shared_ptr<ScopedFd> file, size_t size, size_t offset) {
    if (!loop_->is_in_loop_thread()) {
        std::shared_ptr<TcpConnection> self = shared_from_this();
        loop_->queue_in_loop([self, header, file = std::move(file), size, offset]() {
            self->send_file_with_header_in_loop(header, file, size, offset);
            });
        return;
    }

    send_file_with_header_in_loop(header, std::move(file), size, offset);
}

void TcpConnection::send_file_in_loop(std::shared_ptr<ScopedFd> file, size_t size, size_t offset) {
    assert(loop_->is_in_loop_thread());
    if (isClosed_ || size == 0) {
        return;
    }

    if (!file || !file->valid()) {
        spdlog::error("TcpConnection::send_file_in_loop() got invalid file");
        handle_error_callback();
        close_connection(*channel_);
        return;
    }

    if (!is_regular_file(file->fd())) {
        spdlog::error("TcpConnection::send_file_in_loop() requires a regular file");
        handle_error_callback();
        close_connection(*channel_);
        return;
    }

    if (has_pending_file()) {
        spdlog::error("TcpConnection::send_file_in_loop() already has a pending file");
        handle_error_callback();
        close_connection(*channel_);
        return;
    }

    pendingFile_ = PendingFileSend{ std::move(file), offset, size };
    hasPendingFile_ = true;

    if (writeBuffer_->readable_bytes() > 0 || channel_->is_writing()) {
        channel_->enable_writing();
        return;
    }

    send_pending_file_in_loop();
    if (!has_pending_file()) {
        handle_write_complete_callback();
    }
}

void TcpConnection::send_file_with_header_in_loop(const std::string& header, std::shared_ptr<ScopedFd> file, size_t size, size_t offset) {
    assert(loop_->is_in_loop_thread());
    if (isClosed_) {
        return;
    }

    if (size == 0) {
        send_in_loop(header);
        return;
    }

    if (!file || !file->valid()) {
        spdlog::error("TcpConnection::send_file_with_header_in_loop() got invalid file");
        handle_error_callback();
        close_connection(*channel_);
        return;
    }

    if (!is_regular_file(file->fd())) {
        spdlog::error("TcpConnection::send_file_with_header_in_loop() requires a regular file");
        handle_error_callback();
        close_connection(*channel_);
        return;
    }

    if (has_pending_file()) {
        spdlog::error("TcpConnection::send_file_with_header_in_loop() already has a pending file");
        handle_error_callback();
        close_connection(*channel_);
        return;
    }

    const size_t oldLen = writeBuffer_->readable_bytes();
    writeBuffer_->write_to_buffer(header);
    pendingFile_ = PendingFileSend{ std::move(file), offset, size };
    hasPendingFile_ = true;
    channel_->enable_writing();

    const size_t newLen = writeBuffer_->readable_bytes();
    if (highWaterMarkCallback_ && oldLen < highWaterMark_ && newLen >= highWaterMark_) {
        handle_high_water_mark_callback();
    }
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

    if (writeBuffer_->readable_bytes() > 0) {
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

        if (writeBuffer_->readable_bytes() > 0) {
            return;
        }
    }

    if (has_pending_file()) {
        send_pending_file_in_loop();
        if (has_pending_file()) {
            return;
        }
    }

    channel.disable_writing();
    handle_write_complete_callback();
}

void TcpConnection::send_pending_file_in_loop() {
    assert(loop_->is_in_loop_thread());
    if (!has_pending_file()) {
        return;
    }

    off_t offset = static_cast<off_t>(pendingFile_.offset);
    const ssize_t n = ::sendfile(connSocket_.fd(), pendingFile_.file->fd(), &offset, pendingFile_.remaining);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            channel_->enable_writing();
            return;
        }

        spdlog::error("TcpConnection::send_pending_file_in_loop() failed, errno={} ({})", errno, strerror(errno));
        handle_error_callback();
        close_connection(*channel_);
        return;
    }

    if (n == 0) {
        pendingFile_ = PendingFileSend{};
        hasPendingFile_ = false;
        return;
    }

    const size_t sent = static_cast<size_t>(n);
    pendingFile_.offset = static_cast<size_t>(offset);
    pendingFile_.remaining -= sent;

    if (pendingFile_.remaining == 0) {
        pendingFile_ = PendingFileSend{};
        hasPendingFile_ = false;
        return;
    }

    channel_->enable_writing();
}

void TcpConnection::handle_write_complete_callback() {
    if (!writeCompleteCallback_) {
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
    connSocket_.shutdown_write(); // 先向对端发送 FIN，保证对端看到正常 EOF 而非 RST。
    channel.disable_all();
    handle_close_callback(); // TcpServer 删除连接析构 TcpConnection 对象，TcpConnection 自动管理 Socket、Channel 等资源的生命周期，保证资源正确释放。特别是 Channel 的析构会自动从 Poller 注销，避免悬挂事件。
}

void TcpConnection::handle_close_callback() {
    // shutdown 路径会提前清空 closeCallback_ 以阻断回调链，因此需要判空。
    if (!closeCallback_) {
        return;
    }

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
        return;
    }

    errorCallback_(shared_from_this());
}

void TcpConnection::handle_high_water_mark_callback() {
    if (!highWaterMarkCallback_) {
        return;
    }

    highWaterMarkCallback_(shared_from_this());
}
