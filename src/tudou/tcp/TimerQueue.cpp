// ============================================================================
// TimerQueue.cpp
// TimerQueue 把 timerfd 事件拍平成“读事件、收集到期、执行回调、同步下次唤醒”。
// ============================================================================

#include "TimerQueue.h"

#include <cassert>
#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

#include "Channel.h"
#include "EventLoop.h"
#include "spdlog/spdlog.h"

namespace {

timespec to_timespec(std::chrono::steady_clock::time_point expiration) {
    using namespace std::chrono;

    auto now = steady_clock::now();
    auto duration = expiration - now;
    if (duration < milliseconds(1)) {
        duration = milliseconds(1);
    }

    auto sec = duration_cast<seconds>(duration);
    auto nsec = duration_cast<nanoseconds>(duration - sec);

    timespec ts;
    ts.tv_sec = static_cast<time_t>(sec.count());
    ts.tv_nsec = static_cast<long>(nsec.count());
    return ts;
}

} // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop)
    , timerFd_(create_timerfd())
    , timerChannel_(std::make_unique<Channel>(loop, timerFd_))
    , nextTimerId_(1)
    , timersByExpire_()
    , timersById_() {
    timerChannel_->set_read_callback([this](Channel& channel) { on_timerfd_read(channel); });
    timerChannel_->enable_reading();
}

TimerQueue::~TimerQueue() {
}

TimerId TimerQueue::add_timer(const Timer::Callback& callback, Timestamp when, std::chrono::milliseconds interval) {
    uint64_t id = nextTimerId_++;
    auto timer = std::make_shared<Timer>(id, callback, when, interval);
    loop_->run_in_loop(
        [this, timer]() {
            add_timer_in_loop(timer);
        }
    );
    return TimerId(id);
}

void TimerQueue::erase_timer(TimerId timerId) {
    loop_->run_in_loop(
        [this, timerId]() {
            erase_timer_in_loop(timerId);
        }
    );
}

void TimerQueue::add_timer_in_loop(const std::shared_ptr<Timer>& timer) {
    loop_->assert_in_loop_thread();

    timersByExpire_[{ timer->expiration(), timer->id() }] = timer;
    timersById_[timer->id()] = timer;

    // 注册后统一同步 timerfd，确保首个定时器也会立即接管下一次唤醒时间。
    sync_timerfd();
}

void TimerQueue::erase_timer_in_loop(TimerId timerId) {
    loop_->assert_in_loop_thread();

    auto it = timersById_.find(timerId.value());
    if (it == timersById_.end()) {
        spdlog::warn("TimerQueue::erase_timer_in_loop(): timerId {} not found", timerId.value());
        return;
    }
    const auto timer = it->second;
    timersByExpire_.erase({ timer->expiration(), timer->id() });
    timersById_.erase(it);

    sync_timerfd();
}

void TimerQueue::on_timerfd_read(Channel&) {
    loop_->assert_in_loop_thread();

    // 处理路径固定为“消费事件 -> 抽取到期定时器 -> 执行/重启 -> 同步下次唤醒”。
    read_timerfd(timerFd_);
    const Timestamp now = std::chrono::steady_clock::now();
    const TimerList expiredTimers = collect_expired_timers(now);
    execute_expired_timers(expiredTimers, now);
    sync_timerfd();
}

TimerQueue::TimerList TimerQueue::collect_expired_timers(Timestamp now) {
    TimerList expiredTimers;

    auto it = timersByExpire_.begin();
    while (it != timersByExpire_.end() && it->first.first <= now) {
        expiredTimers.push_back(it->second);
        it = timersByExpire_.erase(it);
    }

    return expiredTimers;
}

void TimerQueue::execute_expired_timers(const TimerList& expiredTimers, Timestamp now) {
    for (const auto& timer : expiredTimers) {
        timer->run();

        // 回调里可能主动取消定时器，所以执行后必须重新检查注册表。
        const auto stillExists = timersById_.find(timer->id());
        if (stillExists == timersById_.end()) {
            continue;
        }

        if (!timer->is_repeat()) {
            timersById_.erase(timer->id());
            continue;
        }

        timer->restart(now);
        reschedule_timer(timer);
    }
}

void TimerQueue::reschedule_timer(const std::shared_ptr<Timer>& timer) {
    timersByExpire_[{ timer->expiration(), timer->id() }] = timer;
}

void TimerQueue::sync_timerfd() {
    if (timersByExpire_.empty()) {
        disarm_timerfd();
        return;
    }

    reset_timerfd(timersByExpire_.begin()->first.first);
}

void TimerQueue::reset_timerfd(Timestamp expiration) {
    itimerspec newValue;
    memset(&newValue, 0, sizeof(newValue));
    newValue.it_value = to_timespec(expiration);

    if (::timerfd_settime(timerFd_, 0, &newValue, nullptr) < 0) {
        spdlog::error("TimerQueue::reset_timerfd() failed, errno={} ({})", errno, strerror(errno));
    }
}

void TimerQueue::disarm_timerfd() {
    itimerspec newValue;
    memset(&newValue, 0, sizeof(newValue));

    if (::timerfd_settime(timerFd_, 0, &newValue, nullptr) < 0) {
        spdlog::error("TimerQueue::disarm_timerfd() failed, errno={} ({})", errno, strerror(errno));
    }
}

int TimerQueue::create_timerfd() {
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        spdlog::critical("TimerQueue::create_timerfd() failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }
    return fd;
}

void TimerQueue::read_timerfd(int timerFd) {
    uint64_t howmany = 0;
    ssize_t n = ::read(timerFd, &howmany, sizeof(howmany));
    if (n != sizeof(howmany)) {
        spdlog::error("TimerQueue::read_timerfd() reads {} bytes instead of 8", n);
    }
}
