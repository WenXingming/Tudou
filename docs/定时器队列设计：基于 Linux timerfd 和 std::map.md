# TimerQueue 设计与实现

`TimerQueue` **把定时器到期转换为 `timerfd` 的可读事件**，再交给 `EventLoop` 统一处理。这样**定时任务可以和网络 I/O 共用同一个事件循环，不需要额外的定时线程**。

> [!note]
> 定时器队列遵循 One-Loop-Per-Thread 模型：数据结构只在所属的 `EventLoop` 线程中修改，跨线程操作通过任务投递完成。队列使用 `std::set` 和 `std::map` 管理定时器，并用 `std::shared_ptr` 保护回调执行期间的对象生命周期。

# Situation — 项目背景

Tudou 是一个基于 Reactor 模式的多线程 C++ 网络框架。连接心跳、空闲超时和延迟任务都依赖定时器。

如果使用独立线程轮询定时器，需要额外的线程和同步机制。因此，TimerQueue 需要直接接入现有的 `epoll` 事件循环，同时支持跨线程添加和取消定时器。

# Task — 任务目标

TimerQueue 需要满足以下要求：

1. 不创建额外的定时线程，复用 `EventLoop` 的事件等待；
2. 外部线程可以安全地添加和取消定时器；
3. 所有容器操作和回调都在所属 `EventLoop` 线程中串行执行；
4. 取消定时器后及时更新 `timerfd`，避免无效唤醒；
5. 处理批量到期回调时，避免自我取消或相互取消造成 Use-After-Free。

# Action — 技术方案与实现

### 底层事件源选型：Linux timerfd

使用 `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)` 创建定时器文件描述符。定时器到期后，`timerfd` 变为可读，`EventLoop` 可以像处理 socket 一样把它注册到 `epoll` 中。

使用 `CLOCK_MONOTONIC` 可以避免系统时间调整影响定时器的时间间隔。

### 双索引数据结构（std::set + std::map）

TimerQueue 维护两份索引：

```text
expireSet_                         timersById_
std::set<TimerEntry>               std::map<TimerId, std::shared_ptr<Timer>>
按到期时间排序                    按 ID 查找
```

- `expireSet_`：`begin()` 指向最早到期的定时器，用于设置下一次 `timerfd` 唤醒时间；
- `timersById_`：根据 `TimerId` 查找定时器，用于取消和执行前的有效性检查。

取消定时器时，先通过 ID 找到对象，再从两个索引中删除。相比 `priority_queue` 的懒删除，`std::set` 可以直接删除中间节点，使队列中只保留有效定时器。

`std::set` 的插入和删除为 `O(log n)`，读取最早元素为 `O(1)`。代价是维护两份索引，但可以精确地同步 `timerfd`：

```cpp
void TimerQueue::sync_timerfd() {
    if (expireSet_.empty()) {
        disarm_timerfd();
        return;
    }
    reset_timerfd(expireSet_.begin()->first);
}
```

### 多线程并发设计（One-Loop-Per-Thread）

`expireSet_` 和 `timersById_` 都不是线程安全容器。设计约定是：只有所属的 `EventLoop` 线程可以访问它们。

其他线程调用 `add_timer` 或 `erase_timer` 时，通过 `run_in_loop()` 投递 Lambda。若 `EventLoop` 正在等待事件，`eventfd` 会负责唤醒它，之后由 EventLoop 线程执行真正的修改。

```cpp
TimerId TimerQueue::add_timer(std::function<void()> callback,
                              Timestamp when,
                              std::chrono::milliseconds interval) {
    TimerId id = TimerId(nextTimerId_.fetch_add(1, std::memory_order_relaxed));
    auto timer = std::make_shared<Timer>(id, std::move(callback), when, interval);

    loop_->run_in_loop([this, timer] {
        expireSet_.insert({timer->get_expiration(), timer});
        timersById_[timer->get_id()] = timer;
        sync_timerfd();
    });
    return id;
}
```

取消定时器采用同样的线程切换方式：

```cpp
void TimerQueue::erase_timer(TimerId timerId) {
    loop_->run_in_loop([this, timerId] {
        auto it = timersById_.find(timerId);
        if (it != timersById_.end()) {
            expireSet_.erase({it->second->get_expiration(), it->second});
            timersById_.erase(it);
        }
        sync_timerfd();
    });
}
```

这样，外部线程可以并发发起请求，但核心容器始终在一个线程内无锁串行访问。

### 生命周期防御：`shared_ptr` 防止“相互取消”崩溃

一次 `timerfd` 事件可能同时触发多个定时器。TimerQueue 会先收集到期定时器，再逐个执行回调。

#### 1. 致命缺陷场景（如果不用 `shared_ptr`）

假设 A 和 B 同时到期：A 的回调取消 B。如果临时列表保存的是裸指针，B 可能在执行前就被释放，之后访问 B 就会产生 Use-After-Free。

#### 2. `shared_ptr` 护栏机制的实现

解决方式是让临时列表保存 `std::shared_ptr<Timer>`：

1. 收集阶段，临时列表持有这一批定时器；
2. 执行每个回调前，检查定时器是否仍在 `timersById_` 中；
3. 已经被取消的定时器只保留对象，不再执行回调；
4. 临时列表销毁后，对象才可能真正释放。

```cpp
void TimerQueue::on_timerfd_read() {
    read_timerfd(timerFd_.fd());

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

    for (const auto& timer : expiredTimers) {
        // 可能已被前一个回调取消
        if (timersById_.find(timer->get_id()) == timersById_.end()) {
            continue;
        }

        timer->run();

        // 可能在自己的回调中取消自己
        if (timersById_.find(timer->get_id()) == timersById_.end()) {
            continue;
        }

        if (!timer->is_repeat()) {
            timersById_.erase(timer->get_id());
            continue;
        }

        timer->reschedule(std::chrono::steady_clock::now());
        expireSet_.insert({timer->get_expiration(), timer});
    }

    sync_timerfd();
}
```

### sync_timerfd 的精确重武装

由于 `expireSet_` 中只保留有效定时器，`sync_timerfd()` 不需要额外清理无效堆顶：

- 集合为空：解除 `timerfd` 武装；
- 集合非空：以最早到期时间重新设置 `timerfd`。

# Result — 重构效果

- 定时器到期检测复用 `EventLoop`，不需要额外的定时线程；
- 跨线程增删通过任务投递完成，核心容器无需加锁；
- `std::set` 支持取消时即时删除，能够及时更新 `timerfd`；
- `shared_ptr` 和执行前检查避免了回调相互取消时的 Use-After-Free；
- 重复定时器在回调结束后重新计算下一次到期时间，避免任务阻塞后集中追赶。

# 核心设计要点提炼 (Key Architectural Points)


| 设计点              | 说明                                                          |
| :-------------------- | :-------------------------------------------------------------- |
| Reactor 集成        | `timerfd` 将定时器到期转换为可读事件，直接接入 `EventLoop`。  |
| One-Loop-Per-Thread | 跨线程只投递任务，索引修改和回调都在所属 EventLoop 线程执行。 |
| 双索引管理          | `std::set` 按到期时间排序，`std::map` 按 ID 查找和取消。      |
| 即时删除            | 取消时同时从两个索引中删除，及时同步`timerfd`。               |
| 生命周期保护        | 临时列表持有`shared_ptr`，并在回调前后检查定时器是否仍有效。  |
| 单调时钟            | 使用`CLOCK_MONOTONIC`，避免系统时间调整影响定时器。           |

# 面试核心问答总结 (Q&A)

## Q1：为什么重构掉 `priority_queue` 懒删除，改用 `std::set`？

`priority_queue` 不支持方便地删除中间元素，通常只能先从 ID 索引中删除，再等堆顶弹出无效节点。这会留下脏数据，并可能造成额外的 `timerfd` 唤醒。

`std::set` 按到期时间排序，取消时可以在 `O(log n)` 时间内直接删除对应节点，因此 `set.begin()` 始终对应有效的最早到期定时器。

## Q2：既然都在同一个线程执行，为什么还会有“相互取消导致的野指针”问题？

单线程只保证回调不会并发执行，但一次事件可能会先收集多个到期定时器，再依次执行它们。

如果 A 的回调取消了 B，而待执行列表保存的是裸指针，B 可能在轮到执行前已经被释放。之后访问 B 就会产生 Use-After-Free。

## Q3：`std::shared_ptr` 是如何具体解决这个崩溃的？

待执行列表保存 `std::shared_ptr<Timer>`，因此即使 B 从 `timersById_` 中删除，待执行列表仍然能保持 B 的对象存活。

执行 B 前再次检查 `timersById_`：如果 B 已经被取消，就跳过回调。当前轮处理结束、临时列表销毁后，B 才会被释放。
