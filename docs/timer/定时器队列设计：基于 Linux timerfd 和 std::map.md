# 定时器队列设计：Linux timerfd、set/map 与回调安全

`TimerQueue` **把定时器到期转换为 `timerfd` 的可读事件**，再交给 `EventLoop` 统一处理。这样**定时任务可以和网络 I/O 共用同一个事件循环，不需要额外的定时线程**。

Tudou 的 `TimerQueue` 不创建独立定时线程，而是把 timerfd 注册为 EventLoop 的一个可读事件。定时器索引和回调都在所属 loop 串行执行，跨线程请求复用 Reactor 的任务投递与唤醒机制。

# Situation — 情境

心跳检测、连接空闲超时和延迟任务都需要定时能力。若额外启动一个轮询线程，它需要与 IO 线程同步共享定时器状态和回调，增加锁、线程切换与生命周期问题。

网络库已经拥有 `epoll` 事件循环，因此更自然的方案是把“到期”转换为一个普通 fd 事件，交给 owner EventLoop 处理。

# Task — 任务

TimerQueue 需要：

1. 不增加额外定时线程，复用 EventLoop；
2. 支持一次性和重复定时器；
3. 根据 `TimerId` 精确取消任务，并及时更新下一次唤醒；
4. 允许其他线程添加或取消，但不让多线程直接修改容器；
5. 正确处理同批任务相互取消、自我取消和重复任务重排。

# Action — 设计与实现

## timerfd 接入 Reactor

TimerQueue 创建非阻塞、close-on-exec 的单调时钟 timerfd，并由 Channel 监听其可读事件：

```cpp
timerFd_ = create_timerfd();
timerChannel_ = std::make_unique<Channel>(loop, timerFd_.fd());
timerChannel_->set_read_callback([this](Channel&) { on_timerfd_read(); });
timerChannel_->enable_reading();

int TimerQueue::create_timerfd() {
    return ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
}
```

`CLOCK_MONOTONIC` 不受系统时间校准影响；timerfd 到期后变为可读，EventLoop 像处理 socket 一样处理它。读取 timerfd 是必须的，否则在当前水平触发语义下会反复收到同一可读事件。

## 双索引：按时间触发，按 ID 取消

```text
expireSet_                              timersById_
std::set<TimerEntry>                    std::map<TimerId, shared_ptr<Timer>>
按 expiration 排序                      按 TimerId 查找
```

`expireSet_.begin()` 始终是有效的最早到期任务，用来设置 timerfd；`timersById_` 用来按 ID 查找和删除。取消时从两份索引同时移除，因此队列中不会留下等待堆顶清理的失效节点。

```cpp
void TimerQueue::sync_timerfd() {
    if (expireSet_.empty()) {
        disarm_timerfd();
        return;
    }
    reset_timerfd(expireSet_.begin()->first);
}
```

代价是插入和删除均为 `O(log n)`，收益是最早到期时间始终准确，取消后能立即重设或解除 timerfd。

## 线程模型：修改统一回到 owner loop

`TimerId` 用 atomic 生成，因此调用者可先得到 ID；真正的容器修改通过 `run_in_loop()` 回到 TimerQueue 所属线程：

```cpp
TimerId id(nextTimerId_.fetch_add(1, std::memory_order_relaxed));
auto timer = std::make_shared<Timer>(id, std::move(callback), when, interval);

loop_->run_in_loop([this, timer] {
    expireSet_.insert({timer->get_expiration(), timer});
    timersById_[timer->get_id()] = timer;
    sync_timerfd();
});
```

跨线程调用会进入 EventLoop 的 pending functors 队列，并由 eventfd 唤醒目标 loop；因此 `set`、`map` 和回调执行始终无需互斥锁。

## 到期批处理与回调取消

timerfd 可读时，TimerQueue 先消费 fd，再摘出所有已到期任务，最后顺序执行：

```cpp
void TimerQueue::on_timerfd_read() {
    read_timerfd(timerFd_.fd());
    auto expiredTimers = collect_expired_timers(std::chrono::steady_clock::now());
    process_expired_timers(expiredTimers);
    sync_timerfd();
}
```

临时列表保存 `shared_ptr<Timer>`，避免 A、B 同时到期时，A 的回调取消 B 后 B 对象在列表遍历中提前释放。执行前后都检查 `timersById_`：

```cpp
for (const auto& timer : expiredTimers) {
    if (timersById_.find(timer->get_id()) == timersById_.end()) {
        continue;
    }
    timer->run();
    if (timersById_.find(timer->get_id()) == timersById_.end()) {
        continue;
    }
    // 一次性任务删除；重复任务重新插入 expireSet_
}
```

重复任务在回调结束后以当前时间重新计算下次到期时间：`expiration = now + interval`。这是固定延迟策略，会有少量漂移，但不会在事件循环卡顿恢复后连续“追赶”多次过期回调。

# Result — 结果

- 定时任务和网络 I/O 共用 EventLoop，不需要额外线程；
- 跨线程增删通过已有任务队列完成，核心容器在线程内无锁；
- `set + map` 同时满足最早到期查询和按 ID 精确取消；
- 批处理列表的 `shared_ptr` 与有效性检查避免回调相互取消导致的悬空访问；
- timerfd 始终指向有效的最早任务，空队列时被解除武装。

# 核心设计要点提炼


| 设计点       | 说明                                              |
| :------------- | :-------------------------------------------------- |
| Reactor 集成 | timerfd 到期变为可读事件，进入同一个 EventLoop。  |
| 时间来源     | `CLOCK_MONOTONIC` 避免墙上时间调整影响间隔。      |
| 双索引       | `set` 管最早到期，`map` 管按 ID 取消。            |
| 线程归属     | 容器修改和回调都在 owner loop；跨线程只投递请求。 |
| 批处理安全   | 临时`shared_ptr` 保活，到执行前后检查是否仍有效。 |
| 重复策略     | 回调后从当前时间重新调度，避免恢复后的追赶风暴。  |

# 面试核心问答总结

## Q1：为什么用 timerfd，而不是单独开一个定时线程？

timerfd 可以注册到 epoll，到期后和 socket 一样作为可读事件处理。这样定时器回调天然落在 owner EventLoop，避免额外线程、锁和跨线程回调。

## Q2：为什么同时使用 set 和 map？

set 按到期时间排序，能快速取得最早任务；map 按 TimerId 查找，能精确取消中间任务。两份索引使取消后可以立即同步 timerfd。

## Q3：同一批到期任务相互取消会发生什么？

临时列表持有 `shared_ptr`，对象不会在遍历中提前析构；每个任务运行前后都会检查它是否仍在 `timersById_`，被取消的任务会被安全跳过。

## Q4：为什么重复定时器不按旧 expiration 累加？

当前采用固定延迟：回调完成后从当前时间重新计时。它牺牲少量时间精度，换取系统卡顿后不连续补跑多次任务，更适合网络服务的保护性行为。
