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

    // 如果新添加的定时器是：1. 第一个定时器；2. 所有定时器中最早到期的一个
    // 重置 timerfd 的到期时间为这个新定时器的到期时间，确保定时器能够及时触发
    bool earliestChanged = timersByExpire_.empty()
        || timer->expiration() < timersByExpire_.begin()->first.first;
    if (earliestChanged) {
        reset_timerfd(timer->expiration());
    }
}

void TimerQueue::erase_timer_in_loop(TimerId timerId) {
    loop_->assert_in_loop_thread();

    auto it = timersById_.find(timerId.value());
    if (it == timersById_.end()) {
        spdlog::warn("TimerQueue::erase_timer_in_loop(): timerId {} not found", timerId.value());
        return;
    }
    auto timer = it->second;
    timersByExpire_.erase({ timer->expiration(), timer->id() });
    timersById_.erase(it);

    // 如果被删除的定时器是所有定时器中最早到期的一个，重置 timerfd 的到期时间为下一个即将到期的定时器的到期时间，确保定时器能够及时触发
    // 这里没有判断删除的定时器是否是最早到期的一个，直接重置 timerfd 的到期时间为下一个即将到期的定时器的到期时间，虽然可能会冗余地调用 reset_timerfd()，但代码更简单
    // 否则用一个变量记录被删除的定时器的到期时间，然后和 timersByExpire_.begin()->first.first 比较即可（<=）
    if (!timersByExpire_.empty()) {
        reset_timerfd(timersByExpire_.begin()->first.first);
    }
}

void TimerQueue::on_timerfd_read(Channel&) {
    loop_->assert_in_loop_thread();

    // 读取 timerfd 数据，重置 timerfd 的可读状态，准备下一次触发
    read_timerfd(timerFd_);

    // 找到所有到期的定时器 ID，执行它们的回调函数，并根据是否重复决定是否重启定时器或删除定时器
    auto now = std::chrono::steady_clock::now();
    std::vector<uint64_t> expiredIds;
    for (auto it = timersByExpire_.begin(); it != timersByExpire_.end() && it->first.first <= now;) {
        expiredIds.push_back(it->first.second);
        it = timersByExpire_.erase(it);
    }

    for (auto id : expiredIds) {
        auto it = timersById_.find(id);
        if (it == timersById_.end()) {
            spdlog::warn("TimerQueue::on_timerfd_read(): timerId {} not found in timersById_", id);
            continue;
        }
        auto timer = it->second;
        timer->run();

        // 二次检查：执行回调函数后，检查定时器是否被取消（可能在回调函数中被取消了）
        auto stillExists = timersById_.find(id);
        if (stillExists == timersById_.end()) {
            continue;
        }

        // 如果没有被取消且是重复定时器，则重启定时器；否则删除定时器
        if (timer->is_repeat()) {
            now = std::chrono::steady_clock::now();
            timer->restart(now);
            timersByExpire_[{ timer->expiration(), timer->id() }] = timer;
        }
        else {
            timersById_.erase(id);
        }
    }

    // 重置 timerfd 的到期时间为下一个即将到期的定时器的到期时间，确保定时器能够及时触发
    // 没有判断下一个即将到期的定时器的到期时间是否改变，直接重置，可能有冗余调用 reset_timerfd()，但代码更简单
    if (!timersByExpire_.empty()) {
        reset_timerfd(timersByExpire_.begin()->first.first);
    }
}

void TimerQueue::reset_timerfd(Timestamp expiration) {
    // 更新底层 Linux timerfd 的下一次超时时间点，以便事件循环能在正确的时间醒来处理定时任务
    itimerspec newValue;
    memset(&newValue, 0, sizeof(newValue));
    newValue.it_value = to_timespec(expiration);

    if (::timerfd_settime(timerFd_, 0, &newValue, nullptr) < 0) {
        spdlog::error("TimerQueue::reset_timerfd() failed, errno={} ({})", errno, strerror(errno));
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
