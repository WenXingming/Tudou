// ============================================================================
// TimerQueue.cpp
// 定时器事件处理管道：读事件 → 收集到期 → 执行回调 → 同步下次唤醒。
// 所有索引操作均在 EventLoop 线程执行，无需加锁。
// ============================================================================

#include "tudou/timer/TimerQueue.h"

#include <cassert>
#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

#include "tudou/reactor/Channel.h"
#include "tudou/reactor/EventLoop.h"
#include "spdlog/spdlog.h"

namespace {

timespec to_timespec(std::chrono::steady_clock::duration duration) {
    using namespace std::chrono;

    if (duration < milliseconds(1)) { // timerfd 不允许 0 延迟，至少设 1ms
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
    , nextTimerId_(1)
    , expireSet_()
    , timersById_()
    , timerFd_(create_timerfd()) {
    timerChannel_ = std::make_unique<Channel>(loop, timerFd_.fd());

    timerChannel_->set_read_callback( // timerfd 到期 → Channel 可读 → on_timerfd_read 接管后续管道
        [this](Channel&) { on_timerfd_read(); }
    );
    timerChannel_->enable_reading();
}

TimerQueue::~TimerQueue() {
    // 成员按声明逆序自动析构：timerChannel_（epoll 注销）→ timerFd_（close fd）。
}

TimerId TimerQueue::add_timer(std::function<void()> callback, Timestamp when, std::chrono::milliseconds interval) {
    TimerId id = TimerId(nextTimerId_.fetch_add(1, std::memory_order_relaxed));
    auto timer = std::make_shared<Timer>(id, std::move(callback), when, interval);

    // 线程安全。索引修改统一回到 EventLoop 线程执行，避免对双索引额外加锁。
    loop_->run_in_loop(
        [this, timer]() {
            expireSet_.insert({ timer->get_expiration(), timer });
            timersById_[timer->get_id()] = timer;
            sync_timerfd();
        }
    );
    return id;
}

void TimerQueue::erase_timer(TimerId timerId) {
    // 线程安全：索引修改统一投递到 EventLoop 线程执行。
    loop_->run_in_loop(
        [this, timerId]() {
            auto it = timersById_.find(timerId);
            if (it != timersById_.end()) {
                expireSet_.erase({ it->second->get_expiration(), it->second });
                timersById_.erase(it);
            }
            sync_timerfd();
        }
    );
}

// 管道四步：消费 → 收集 → 执行 → 同步
void TimerQueue::on_timerfd_read() {
    read_timerfd(timerFd_.fd());

    // 收集到期定时器：只从排序容器 expireSet_ 中移除，保留在 timersById_ 中用于取消校验。只有 erase_timer 才能删除定时器
    const Timestamp now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<Timer>> expiredTimers;
    while (!expireSet_.empty()) {
        auto it = expireSet_.begin();
        if (it->first > now) {
            break;
        }
        expiredTimers.push_back(it->second);
        expireSet_.erase(it);
    }

    // 执行阶段
    for (const auto& timer : expiredTimers) {
        // 执行前检查是否已被前置的回调取消
        if (timersById_.find(timer->get_id()) == timersById_.end()) {
            continue;
        }

        timer->run();

        // 执行后再次确认（防止在它自己的回调中取消了自己）
        if (timersById_.find(timer->get_id()) == timersById_.end()) {
            continue;
        }

        if (!timer->is_repeat()) {
            timersById_.erase(timer->get_id());
            continue;
        }

        // 重复定时器以“当前执行完成的时刻”为基准推进下一次到期时间。而非 expiration_ = expiration_ + interval_ 精确定时器（触发频率固定）
        // 虽然这会累积微量的时间漂移（线程调度延迟 + 回调函数执行时间），但可以有效防止在系统卡顿恢复后发生定时任务的“追赶风暴”（即连续多次触发冗余回调）
        const Timestamp t = std::chrono::steady_clock::now();
        timer->reschedule(t);
        expireSet_.insert({ timer->get_expiration(), timer });
    }

    sync_timerfd();
}

void TimerQueue::sync_timerfd() {
    // 无定时器：解除武装，避免 timerfd 空唤醒浪费 CPU
    if (expireSet_.empty()) {
        disarm_timerfd();
        return;
    }
    // 以最早到期时间重新武装 timerfd
    reset_timerfd(expireSet_.begin()->first);
}

void TimerQueue::reset_timerfd(Timestamp expiration) {
    itimerspec newValue;
    memset(&newValue, 0, sizeof(newValue));
    auto duration = expiration - std::chrono::steady_clock::now();
    newValue.it_value = to_timespec(duration);
    if (::timerfd_settime(timerFd_.fd(), 0, &newValue, nullptr) < 0) {
        spdlog::error("TimerQueue::reset_timerfd() failed, errno={} ({})", errno, strerror(errno));
    }
}

void TimerQueue::disarm_timerfd() {
    itimerspec newValue;
    memset(&newValue, 0, sizeof(newValue)); // it_value = {0, 0} 表示不再触发到期事件
    if (::timerfd_settime(timerFd_.fd(), 0, &newValue, nullptr) < 0) {
        spdlog::error("TimerQueue::disarm_timerfd() failed, errno={} ({})", errno, strerror(errno));
    }
}

int TimerQueue::create_timerfd() {
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); // 使用 MONOTONIC + NONBLOCK + CLOEXEC，保证时间稳定且 fd 生命周期清晰。
    if (fd < 0) {
        spdlog::critical("TimerQueue::create_timerfd() failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }
    return fd;
}

// timerfd 到期后变为可读，必须 read 消费事件，否则 epoll 会反复通知（LT 模式）
void TimerQueue::read_timerfd(int timerFd) {
    uint64_t howmany = 0;
    ssize_t n = ::read(timerFd, &howmany, sizeof(howmany));
    if (n != sizeof(howmany)) {
        spdlog::error("TimerQueue::read_timerfd() reads {} bytes instead of 8", n);
    }
}
