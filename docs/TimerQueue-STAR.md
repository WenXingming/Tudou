# TimerQueue 设计与实现（STAR 法则）

## Situation — 项目背景

Tudou 是一个基于 Reactor 模式的多线程 C++ 网络框架。在实现 TCP 连接的心跳机制时，需要定时器基础设施支持——连接的保活探测、空闲超时断开等功能都依赖定时器。框架需要在**不引入额外线程**的前提下，提供高效、线程安全的定时器管理能力，且必须与现有的 epoll 事件循环无缝集成。

## Task — 任务目标

设计并实现一个定时器队列组件，满足以下约束：

1. **零额外线程**：定时器到期检测不能使用独立的定时器线程，必须复用 EventLoop 的 epoll 轮询
2. **线程安全**：允许任意线程添加/取消定时器，但实际执行必须在 EventLoop 线程
3. **高效操作**：添加、取消、查找最早到期定时器的复杂度需可控
4. **支持重复定时器**：如心跳探测需要按固定间隔重复触发

## Action — 技术方案与实现

### 1. 底层机制选型：Linux timerfd

使用 `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)` 创建定时器文件描述符。timerfd 在到期时变为可读，可被 epoll 多路复用——和 socket I/O 统一为一个事件处理循环。

```
timerfd_settime(fd, 0, &newValue, nullptr)
        │
        ▼
  epoll_wait 返回 timerfd 可读事件
        │
        ▼
  Channel 回调 → on_timerfd_read()
```

对比方案：若使用独立的 `std::thread` + `sleep` 循环，需要线程同步且浪费一个线程；若使用 `setitimer` + 信号，信号处理受限且与多线程模型冲突。timerfd 是最契合 Reactor 模型的选择。

### 2. 双索引数据结构

```
timersByExpire_                          timersById_
map<pair<Timestamp, TimerId>, Timer>     map<TimerId, Timer>

主索引：按到期时间排序                   辅助索引：按 ID 排序
用途：sync_timerfd() O(1)              用途：erase_timer() O(log n)
      collect_expired_timers() O(k)          execute中存活检查 O(log n)
```

两个 map 指向同一批 `shared_ptr<Timer>` 对象。单一按时间排序的索引无法高效支持按 ID 取消（需 O(n) 遍历）；单一按 ID 排序的索引无法快速找到最早到期者（同样 O(n)）。双索引以一倍内存开销换取所有操作都在 O(log n) 以内。

### 3. 线程安全模型：one loop per thread

所有索引操作（增删改查）都在 EventLoop 线程执行，**无锁设计**。外部线程通过 `loop_->run_in_loop(lambda)` 将操作投递到 EventLoop 线程：

```cpp
TimerId add_timer(const Callback& cb, Timestamp when, milliseconds interval) {
    TimerId id = TimerId(nextTimerId_++);
    auto timer = std::make_shared<Timer>(id, cb, when, interval);
    loop_->run_in_loop([this, timer]() {
        // 此 lambda 在 EventLoop 线程执行，无需加锁
        timersByExpire_[{ timer->expiration(), timer->id() }] = timer;
        timersById_[timer->id()] = timer;
        sync_timerfd();  // 新定时器可能成为最早到期的，需重新武装 timerfd
    });
    return id;
}
```

### 4. 到期处理流程

```
on_timerfd_read()
  ├─ read_timerfd()           // 消费事件（读 8 字节）
  ├─ collect_expired_timers() // 从 timersByExpire_ 头部收集所有 expiration <= now
  ├─ execute_expired_timers() // 逐一执行回调
  │    ├─ timer->run()        // 执行用户回调
  │    ├─ 检查回调是否取消了自身（timersById_ 中查找）
  │    ├─ 一次性定时器：从 timersById_ 移除
  │    └─ 重复定时器：timer->restart(now)，重新插入 timersByExpire_
  └─ sync_timerfd()           // 根据新的最早到期时间重新武装 timerfd
```

关键细节：`execute_expired_timers` 中每个定时器执行后重新检查 `timersById_`——因为用户回调可能调用了 `cancel()` 将后续定时器一并移除。

### 5. sync_timerfd 的武装/解除逻辑

```cpp
void sync_timerfd() {
    if (timersByExpire_.empty()) {
        disarm_timerfd();  // it_value = {0,0}，timerfd 不再触发
    } else {
        reset_timerfd(timersByExpire_.begin()->first.first);  // 武装到最早到期时间
    }
}
```

每次添加或移除定时器后都必须调用 `sync_timerfd()`，确保 timerfd 始终反映当前最早到期时间——旧定时器被取消而新定时器更晚时，如果忘记解除则会导致空唤醒；新定时器更早时如果忘记重新武装则会延迟触发。

## Result — 效果

- **零线程开销**：定时器完全融入 epoll 事件循环，不需要额外的定时器线程
- **操作复杂度**：添加 O(log n)，取消 O(log n)，获取最早到期 O(1)，批量收集到期 O(k)
- **线程安全**：所有索引操作在 EventLoop 线程无锁执行，外部线程通过 `run_in_loop` 投递
- **可扩展**：框架内所有定时需求（心跳保活、连接超时、延迟任务）共用同一套机制
- **正确性**：`execute_expired_timers` 中重新检查存活状态，避免回调中取消定时器导致悬空访问

---

## 扩展思考：为什么很多类持有 `EventLoop*`？

持有 `EventLoop*` 的类：`Channel`、`EpollPoller`、`TimerQueue`、`Acceptor`、`TcpConnection`。三个核心用途：

**1. 线程归属断言** — 所有关键方法都在 `assert_in_loop_thread()` 守卫下执行，多线程误用立即 crash，提前暴露 bug。

**2. 跨线程任务投递** — 外部线程通过 `loop_->run_in_loop(lambda)` 将操作安全转移到 EventLoop 线程。

**3. 间接访问同线程设施** — 如 `Channel → EventLoop → EpollPoller`，避免跨层直接持有指针，保持相邻类通信原则。

本质：EventLoop 是线程内的**唯一协调者**，是所有同线程对象的调度中心。持有 `EventLoop*` 就是持有"我在哪个线程、我能调度什么"的上下文。

---

# TimerQueue 深度源码分析报告

## 一、 核心宏观干道 (Macro Execution Pathway)

### 1.1 添加定时器（跨线程安全入口）

```
外部线程调用
    └── TimerQueue::add_timer(callback, when, interval)
            ├── TimerId id = TimerId(nextTimerId_++)            # ① 在调用方线程生成唯一 ID，保证同步返回
            ├── std::make_shared<Timer>(id, cb, when, interval) # 构造值对象，不参与调度
            └── loop_->run_in_loop(lambda)                      # ② 投递到 EventLoop 线程
                    └── [lambda 在 EventLoop 线程执行]
                            ├── timersByExpire_[{expiration, id}] = timer  # ③ 插入主索引（按时间排序）
                            ├── timersById_[id] = timer                     # ③ 插入辅助索引（按 ID 排序）
                            └── sync_timerfd()                              # ④ 武装 timerfd
                                    ├── timersByExpire_ 为空? → disarm_timerfd()    # 解除武装
                                    └── 否则 → reset_timerfd(最早到期时间)           # 重设 timerfd
                                            └── ::timerfd_settime(timerFd_, ...)      # 系统调用
```

### 1.2 取消定时器（跨线程安全入口）

```
外部线程调用
    └── TimerQueue::erase_timer(timerId)
            └── loop_->run_in_loop(lambda)                      # 投递到 EventLoop 线程
                    └── [lambda 在 EventLoop 线程执行]
                            ├── loop_->assert_in_loop_thread()               # 线程归属断言
                            ├── timersById_.find(timerId)                   # ① 按 ID 查找
                            │       └── 未找到 → warn + return
                            ├── timersByExpire_.erase({expiration, id})     # ② 从时间索引移除
                            ├── timersById_.erase(it)                       # ② 从 ID 索引移除
                            └── sync_timerfd()                              # ③ 最早到期可能已变，重新同步
```

### 1.3 定时器到期处理管道（EventLoop 线程内，单线程无锁）

```
epoll_wait 返回 timerFd_ 可读事件
    └── Channel::handle_events()                              # Channel 分发就绪事件
            └── read_callback → TimerQueue::on_timerfd_read()  # 管道唯一入口
                    ├── read_timerfd(timerFd_)                  # ① 消费事件（读 8 字节）
                    │       └── ::read(timerFd_, &howmany, 8)   # 必须消费，否则 LT 模式反复通知
                    ├── collect_expired_timers(now)             # ② 收集到期定时器
                    │       └── while (最早定时器 <= now):
                    │               ├── expiredTimers.push_back(timer)
                    │               └── timersByExpire_.erase(it)  # 从主索引移除（重复定时器稍后重新插入）
                    ├── execute_expired_timers(expired, now)    # ③ 逐条执行
                    │       └── for each timer in expired:
                    │               ├── timer->run()                         # 执行用户回调
                    │               │       └── callback_()                  # 回调内可能调用 cancel() 取消自身或其它定时器
                    │               ├── timersById_.find(timer->id())        # 回调后重新检查存活
                    │               │       └── 不存在 → continue            # 已被回调取消，跳过
                    │               ├── !timer->is_repeat()?                 # 一次性定时器
                    │               │       └── timersById_.erase(timer->id())  # 清理 ID 索引
                    │               └── timer->is_repeat()?                 # 重复定时器
                    │                       ├── timer->restart(now)           # 以当前时间为基准推进到期时间
                    │                       └── timersByExpire_[{newExp, id}] = timer  # 重新插入主索引
                    └── sync_timerfd()                          # ④ 根据新的最早到期重新武装
                            ├── timersByExpire_ 为空? → disarm_timerfd()
                            └── 否则 → reset_timerfd(最早到期时间)
```

关键点: 所有索引操作都在 EventLoop 线程执行，零锁开销。外部线程通过 `run_in_loop` 将操作投递到正确线程，保证线程安全。

---

## 二、 架构与数据流转图 (Architecture & Data Flow Diagram)

```
                         外部线程 (可多个)                       EventLoop 线程 (唯一)
                         ──────────────                        ─────────────────────
                              │                                          │
              add_timer()     │     erase_timer()                        │
              ┌───────┴───────┴───────┐                                  │
              │  run_in_loop(lambda)  │  # 跨线程投递                    │
              └───────────┬───────────┘                                  │
                          │                                              │
                          v                                              │
              +-------------------------------------------------------+  │
              |                   TimerQueue                          |  │
              |                                                       |  │
              |  +-- timerFd_ ──────────> epoll 监听                  |  │
              |  +-- timerChannel_ ──────> 回调 → on_timerfd_read()   |  │
              |  +-- nextTimerId_ ───────> 64位自增计数器              |  │
              |                                                       |  │
              |  +-- timersByExpire_ ────> map< <Timestamp, TimerId>, |  │
              |  |    (主索引: 按时间)        shared_ptr<Timer> >      |  |
              |  |                         ┌─────────────────┐        |  |
              |  |                         | TimerA (T+100ms) │        |  |
              |  |                         | TimerB (T+200ms) │        |  |
              |  |                         | TimerC (T+500ms) │        |  |
              |  |                         └─────────────────┘        |  |
              |  |                              |                      |  |
              |  |                    同一批 shared_ptr 对象            |  |
              |  |                              |                      |  |
              |  +-- timersById_ ────────> map< TimerId,              |  |
              |       (辅索引: 按ID)           shared_ptr<Timer> >     |  |
              |                               ┌─────────────┐         |  |
              |                               | id=1: TimerA |         |  |
              |                               | id=2: TimerB |         |  |
              |                               | id=3: TimerC |         |  |
              |                               └─────────────┘         |  |
              +-------------------------------------+-----------------+  |
                                                    |                    |
                          timerfd 到期 (epoll 通知)  |                    |
                                                    v                    |
                              on_timerfd_read() ─────────────────────────┘
                                      │
                    ┌─────────────────┼─────────────────┐
                    │                 │                 │
                    v                 v                 v
            read_timerfd()   collect_expired()  execute_expired()
              (消费事件)       (收集到期)        (执行回调+清理)


            数据流向 (add_timer):
            ==================
            调用方线程                    EventLoop 线程
            ─────────                    ──────────────
            TimerId id                     │
                 │                         │
            Timer obj ──run_in_loop──> timersByExpire_[{when, id}]
                                        timersById_[id]
                                             │
                                        sync_timerfd()
                                             │
                                    timerfd_settime(timerFd_, 最早到期)


            数据流向 (到期触发):
            ==================
            timerfd 可读 (epoll)
                 │
                 v
            on_timerfd_read()
                 │
         ┌───────┼──────────┐
         v       v          v
      消费    收集到期    逐条执行
     事件    (从主索引     ├─ run()
             移除并收集)   ├─ 检查存活 (辅索引)
                          ├─ 一次性: 清理辅索引
                          └─ 重复: restart + 回插主索引
                 │
                 v
           sync_timerfd()
           (武装/解除)
```

---

## 三、 关键数据结构详解

### 3.1 TimerKey — 主索引的复合键

```cpp
using TimerKey = std::pair<Timestamp, TimerId>;
//                 ^^^^^^^^^  ^^^^^^^
//                 到期时间    唯一ID（解决同一时间戳冲突）
```

`std::pair` 的默认比较先比 first 再比 second，天然实现了"按到期时间排序，同一时间按 ID 区分"。TimerId 作为 second 确保即使两个定时器设了相同的到期时间，map 也不会把它们当同一个 key 覆盖。

### 3.2 双索引指向同一批对象

```
timersByExpire_:  map<TimerKey, shared_ptr<Timer>>   // 主索引
timersById_:      map<TimerId,  shared_ptr<Timer>>   // 辅索引

                           ┌──────────────────┐
                      ┌──→ │ Timer (id=1)     │
  timersByExpire_ ────┤    │ expiration=T+100 │
  [{T+100, id=1}]      │    └──────────────────┘
                       │
  timersById_ ─────────┤    ┌──────────────────┐
  [id=1]               └──→ │ Timer (id=2)     │
                            │ expiration=T+200 │
                            └──────────────────┘
```

两个 map 的 value 是**同一个** `shared_ptr<Timer>`，所以引用计数自然管理了对象生命周期——两个索引都移除后，Timer 自动析构。

### 3.3 各索引的操作复杂度

| 操作 | timersByExpire_ | timersById_ | 说明 |
|------|:---:|:---:|------|
| 查找最早到期 | O(1) | O(n) | `begin()` 即最早 |
| 按 ID 查找 | O(n) | O(log n) | 辅索引按 ID 排序 |
| 批量收集到期 | O(k) | — | k 为到期数量，顺序遍历 |
| 插入 | O(log n) | O(log n) | 两个 map 各插入一次 |
| 删除 | O(log n) | O(log n) | 两个 map 各删除一次 |

---

## 四、 核心设计要点提炼 (Key Architectural Points)

| 设计点 (Design Pattern / Principle) | 代码体现与说明 |
|---|---|
| **Reactor 集成 (timerfd + epoll)** | `timerfd_create` 将时间事件转化为 fd 可读事件，统一纳入 epoll 多路复用，无需独立定时器线程。`Channel(timerFd_)` 注册读回调 `on_timerfd_read`，完全融入 EventLoop 事件循环 |
| **门面模式 (Facade)** | `TimerQueue` 对外只暴露 `add_timer` / `erase_timer` 两个公开接口，隐藏内部双索引、timerfd 系统调用、线程投递等所有实现细节。调用方（EventLoop、TcpConnection）无需感知 timerfd 的存在 |
| **单一职责原则 (SRP)** | `Timer` 只保存单个定时器的到期时间和回调，不参与调度；`TimerQueue` 只负责编排调度和 timerfd 管理；`Channel` 只负责 fd 事件分发。三层职责清晰分割 |
| **one loop per thread 并发模型** | 所有索引操作限定在 EventLoop 线程执行（`assert_in_loop_thread` 守卫），零锁设计。跨线程访问通过 `loop_->run_in_loop(lambda)` 投递，利用 eventfd 唤醒机制立即执行 |
| **依赖注入 (Dependency Injection)** | `TimerQueue(EventLoop* loop)` 通过构造函数注入所属 EventLoop，不持有所有权（裸指针），符合"生命周期由智能指针管理，访问用裸指针"的项目规范 |
| **双索引冗余设计 (Space-for-Time)** | 以双倍指针内存为代价，将按时间查找和按 ID 查找都优化到 O(log n)，避免在单一索引上做 O(n) 遍历。两个索引指向同一批 `shared_ptr<Timer>`，引用计数自动管理生命周期 |
| **回调后存活检查 (Defensive Check)** | `execute_expired_timers` 中每条定时器执行后重新 `timersById_.find(id)`，因为回调内可能通过 `cancel()` 移除了其他定时器。这是一种防御式编程，避免访问已释放对象 |
| **RAII 管理 fd 生命周期** | `timerFd_` 由 `TimerQueue` 构造函数创建，`timerChannel_`（`unique_ptr<Channel>`）析构时自动 `::close(fd)`。timerfd 的创建和销毁与 TimerQueue 生命周期严格绑定 |
| **解除武装避免空唤醒** | `sync_timerfd` 中队列为空时调用 `disarm_timerfd` 设置 `it_value={0,0}`，避免 timerfd 在没有定时器时仍然触发，浪费 epoll 唤醒的 CPU 开销 |
| **CLOCK_MONOTONIC 时钟源** | `timerfd_create(CLOCK_MONOTONIC, ...)` 使用单调时钟，不受系统时间调整（如 NTP 校时、管理员手动改时间）影响，保证定时器行为可预测 |

---

## 五、 面试核心要点总结

1. **为什么用 timerfd 而不是独立线程？** — 融入 epoll 事件循环，零额外线程开销，统一的事件处理模型。
2. **为什么需要双索引？** — 单索引无法同时高效支持"按时间找最早到期"和"按 ID 取消"。双索引以空间换时间，所有操作 O(log n)。
3. **如何保证线程安全？** — one loop per thread 模型，所有索引操作在 EventLoop 线程执行，外部线程通过 `run_in_loop` 投递，eventfd 唤醒。
4. **重复定时器如何实现？** — `execute_expired_timers` 中检查 `is_repeat()`，调用 `restart(now)` 推进到期时间，重新插入主索引。
5. **回调中取消定时器安全吗？** — 安全。`execute_expired_timers` 在每次 `run()` 后重新检查 `timersById_`，被取消的定时器会被跳过。
6. **timerfd 延迟最小 1ms 是怎么保证的？** — `to_timespec` 中 `if (duration < 1ms) duration = 1ms`，因为 timerfd 不接受 0 延迟。
