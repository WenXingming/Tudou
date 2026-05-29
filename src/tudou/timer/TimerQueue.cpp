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
    , nextTimerId_(1)
    , expireHeap_()
    , timersById_() {
    // timerFd_ 的 in-class 初始化器 {-1} 先于构造函数体执行，此处赋值触发 Socket 移动赋值接管 fd。
    timerFd_ = Socket(create_timerfd());
    timerChannel_ = std::make_unique<Channel>(loop, timerFd_.fd());
    // timerfd 到期 → Channel 可读 → on_timerfd_read 接管后续管道
    timerChannel_->set_read_callback([this](Channel&) { on_timerfd_read(); });
    timerChannel_->enable_reading();
}

TimerQueue::~TimerQueue() {
    // 成员按声明逆序自动析构：timerChannel_（epoll 注销）→ timerFd_（close fd）。
}

TimerId TimerQueue::add_timer(std::function<void()> callback, Timestamp when, std::chrono::milliseconds interval) {
    TimerId id = TimerId(nextTimerId_.fetch_add(1, std::memory_order_relaxed));
    auto timer = std::make_shared<Timer>(id, std::move(callback), when, interval);

    // 索引修改统一回到 EventLoop 线程执行，避免对双索引额外加锁。
    loop_->run_in_loop(
        [this, timer]() {
            expireHeap_.push({ timer->expiration(), timer->id() });
            timersById_[timer->id()] = timer;
            sync_timerfd();
        }
    );
    return id;
}

void TimerQueue::erase_timer(TimerId timerId) {
    loop_->run_in_loop(
        [this, timerId]() {
            timersById_.erase(timerId); // 如果不存在这个 ID，erase 不做任何事，返回值的语义是删除的元素个数
            sync_timerfd();
        }
    );
}

void TimerQueue::on_timerfd_read() {
    // 管道四步：消费 → 收集 → 执行 → 同步
    read_timerfd(timerFd_.fd());

    const Timestamp now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<Timer>> expiredTimers;
    while (!expireHeap_.empty()) {
        auto top = expireHeap_.top();
        // 懒删除的定时器
        TimerId timerId = top.second;
        if (timersById_.find(timerId) == timersById_.end()) {
            expireHeap_.pop();
            continue;
        }
        // 收集到期的定时器
        if (top.first > now) {
            break;
        }
        expiredTimers.push_back(timersById_[timerId]);
        expireHeap_.pop();
    }

    for (const auto& timer : expiredTimers) {
        timer->run();

        // 回调中可能连带取消定时器（包括自己），因此执行后要重新确认它是否还在索引里。
        const auto stillExists = timersById_.find(timer->id());
        if (stillExists == timersById_.end()) {
            continue;
        }
        // 如果是一次性定时器，回调后就从索引里删除；如果是重复定时器，重启并重新加入到期队列。
        if (!timer->is_repeat()) {
            timersById_.erase(timer->id());
            continue;
        }
        // 重复定时器以“当前执行完成的时刻”为基准推进下一次到期时间，避免回调执行时间过长导致的时间漂移。
        const Timestamp t = std::chrono::steady_clock::now();
        timer->restart(t);
        expireHeap_.push({ timer->expiration(), timer->id() });
    }

    sync_timerfd();
}

void TimerQueue::sync_timerfd() {
    // 无定时器：解除武装，避免 timerfd 空唤醒浪费 CPU
    if (expireHeap_.empty()) {
        disarm_timerfd();
        return;
    }
    // 以最早到期时间重新武装 timerfd
    reset_timerfd(expireHeap_.top().first);
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
