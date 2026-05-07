// ============================================================================
// TimerQueue.cpp
// 定时器事件处理管道：读事件 → 收集到期 → 执行回调 → 同步下次唤醒。
// 所有索引操作均在 EventLoop 线程执行，无需加锁。
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
    // timerfd 不允许 0 延迟，至少设 1ms
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
    // timerfd 到期 → Channel 可读 → on_timerfd_read 接管后续管道
    timerChannel_->set_read_callback([this](Channel&) { on_timerfd_read(); });
    timerChannel_->enable_reading();
}

TimerQueue::~TimerQueue() {
    // Channel 不再负责关闭 fd，析构时显式清理 timerfd
    timerChannel_.reset();
    ::close(timerFd_);
}

TimerId TimerQueue::add_timer(const Timer::Callback& callback, Timestamp when, std::chrono::milliseconds interval) {
    TimerId id = TimerId(nextTimerId_++);
    auto timer = std::make_shared<Timer>(id, callback, when, interval);

    // 索引修改统一回到 EventLoop 线程执行，避免对双索引额外加锁。
    loop_->run_in_loop(
        [this, timer]() {
            timersByExpire_[{ timer->expiration(), timer->id() }] = timer;
            timersById_[timer->id()] = timer;
            sync_timerfd();
        }
    );
    return id;
}

void TimerQueue::erase_timer(TimerId timerId) {
    loop_->run_in_loop(
        [this, timerId]() {
            auto it = timersById_.find(timerId);
            if (it == timersById_.end()) {
                spdlog::warn("TimerQueue::erase_timer(): timerId {} not found", timerId.value());
                return;
            }

            const auto timer = it->second;
            timersByExpire_.erase({ timer->expiration(), timer->id() });
            timersById_.erase(it);
            sync_timerfd();
        }
    );
}

void TimerQueue::on_timerfd_read() {
    // 管道四步：消费 → 收集 → 执行 → 同步
    read_timerfd(timerFd_);

    const Timestamp now = std::chrono::steady_clock::now();
    const TimerList expiredTimers = collect_expired_timers(now);

    execute_expired_timers(expiredTimers, now);
    sync_timerfd();
}

TimerQueue::TimerList TimerQueue::collect_expired_timers(Timestamp now) {
    TimerList expiredTimers;

    // timersByExpire_ 按到期时间升序，从头遍历直到遇到未到期的
    auto it = timersByExpire_.begin();
    while (it != timersByExpire_.end() && it->first.first <= now) {
        expiredTimers.push_back(it->second);
        it = timersByExpire_.erase(it); // 先从主索引移除，按需在 execute 中重新插入
    }

    return expiredTimers;
}

void TimerQueue::execute_expired_timers(const TimerList& expiredTimers, Timestamp now) {
    for (const auto& timer : expiredTimers) {
        timer->run();

        // 回调中可能连带取消其他定时器，因此执行后要重新确认它是否还在索引里。
        const auto stillExists = timersById_.find(timer->id());
        if (stillExists == timersById_.end()) {
            continue;
        }

        if (!timer->is_repeat()) {
            timersById_.erase(timer->id());
            continue;
        }

        timer->restart(now);
        timersByExpire_[{ timer->expiration(), timer->id() }] = timer;
    }
}

void TimerQueue::sync_timerfd() {
    if (timersByExpire_.empty()) {
        // 无定时器：解除武装，避免 timerfd 空唤醒浪费 CPU
        disarm_timerfd();
        return;
    }

    // 以最早到期时间重新武装 timerfd
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
    // it_value = {0, 0} 表示不再触发到期事件
    itimerspec newValue;
    memset(&newValue, 0, sizeof(newValue));

    if (::timerfd_settime(timerFd_, 0, &newValue, nullptr) < 0) {
        spdlog::error("TimerQueue::disarm_timerfd() failed, errno={} ({})", errno, strerror(errno));
    }
}

int TimerQueue::create_timerfd() {
    // 使用 MONOTONIC + NONBLOCK + CLOEXEC，保证时间稳定且 fd 生命周期清晰。
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        spdlog::critical("TimerQueue::create_timerfd() failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }
    return fd;
}

void TimerQueue::read_timerfd(int timerFd) {
    // timerfd 到期后变为可读，必须 read 消费事件，否则 epoll 会反复通知（LT 模式）
    uint64_t howmany = 0;
    ssize_t n = ::read(timerFd, &howmany, sizeof(howmany));
    if (n != sizeof(howmany)) {
        spdlog::error("TimerQueue::read_timerfd() reads {} bytes instead of 8", n);
    }
}
