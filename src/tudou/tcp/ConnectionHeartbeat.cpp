// ============================================================================
// ConnectionHeartbeat.cpp
// 通用连接空闲检测实现，周期性检查空闲超时并在超时后关闭连接。
// ============================================================================

#include "tudou/tcp/ConnectionHeartbeat.h"

#include <cassert>
#include <chrono>

#include "spdlog/spdlog.h"

#include "tudou/reactor/EventLoop.h"
#include "tudou/tcp/TcpConnection.h"

ConnectionHeartbeat::ConnectionHeartbeat(const std::shared_ptr<TcpConnection>& conn,
    double checkIntervalSeconds,
    double idleTimeoutSeconds) :

    loop_(conn ? conn->get_loop() : nullptr),
    checkIntervalSeconds_(checkIntervalSeconds),
    idleTimeoutSeconds_(idleTimeoutSeconds),
    lastActiveTime_(std::chrono::steady_clock::now()),
    timerId_(),
    conn_(conn),
    running_(false) {

}

void ConnectionHeartbeat::start() {
    if (!loop_->is_in_loop_thread()) {
        std::shared_ptr<ConnectionHeartbeat> self = shared_from_this();
        loop_->queue_in_loop([self]() {
            self->start_in_loop();
            });
        return;
    }

    start_in_loop();
}

void ConnectionHeartbeat::start_in_loop() {
    assert(loop_ != nullptr);
    assert(loop_->is_in_loop_thread());

    if (running_) {
        stop_in_loop();
    }

    if (checkIntervalSeconds_ <= 0.0 || idleTimeoutSeconds_ <= 0.0) {
        auto conn = conn_.lock();
        spdlog::warn("ConnectionHeartbeat::start() invalid args, checkInterval={}, idleTimeout={}, fd={}",
            checkIntervalSeconds_,
            idleTimeoutSeconds_,
            conn ? conn->get_fd() : -1);
        return;
    }

    lastActiveTime_ = std::chrono::steady_clock::now();

    std::weak_ptr<ConnectionHeartbeat> weakHeartbeat(shared_from_this());
    timerId_ = loop_->run_every(checkIntervalSeconds_, [weakHeartbeat]() {
        auto heartbeat = weakHeartbeat.lock();
        if (!heartbeat) {
            return;
        }
        heartbeat->check_timeout();
        });

    running_ = true;
}

void ConnectionHeartbeat::stop() {
    if (!loop_->is_in_loop_thread()) {
        std::shared_ptr<ConnectionHeartbeat> self = shared_from_this();
        loop_->queue_in_loop([self]() {
            self->stop_in_loop();
            });
        return;
    }

    stop_in_loop();
}

void ConnectionHeartbeat::stop_in_loop() {
    assert(loop_ != nullptr);
    assert(loop_->is_in_loop_thread());

    running_ = false;
    if (!timerId_.valid()) {
        return;
    }

    loop_->cancel(timerId_);
    timerId_ = TimerId();
}

void ConnectionHeartbeat::refresh() {
    if (!loop_->is_in_loop_thread()) {
        std::shared_ptr<ConnectionHeartbeat> self = shared_from_this();
        loop_->queue_in_loop([self]() {
            self->refresh_in_loop();
            });
        return;
    }

    refresh_in_loop();
}

void ConnectionHeartbeat::refresh_in_loop() {
    assert(loop_ != nullptr);
    assert(loop_->is_in_loop_thread());
    lastActiveTime_ = std::chrono::steady_clock::now();
}

void ConnectionHeartbeat::check_timeout() {
    assert(loop_ != nullptr);
    assert(loop_->is_in_loop_thread());

    if (!running_) {
        return;
    }

    auto conn = conn_.lock();
    if (!conn) {
        stop_in_loop();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!is_timeout(now)) {
        return;
    }

    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActiveTime_).count();
    const auto timeoutMs = static_cast<long long>(idleTimeoutSeconds_ * 1000.0);
    spdlog::warn("ConnectionHeartbeat timeout, fd={}, idleMs={}, timeoutMs={}", conn->get_fd(), idleMs, timeoutMs);
    conn->force_close();
}

bool ConnectionHeartbeat::is_timeout(std::chrono::steady_clock::time_point now) const {
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActiveTime_).count();
    const auto timeoutMs = static_cast<long long>(idleTimeoutSeconds_ * 1000.0);
    return idleMs >= timeoutMs;
}