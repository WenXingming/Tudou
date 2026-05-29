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

### 2. 双索引数据结构（优先队列 + 懒删除）

```
expireHeap_                              timersById_
priority_queue<TimerKey, greater<>>          map<TimerId, shared_ptr<Timer>>

主索引：按到期时间最小堆                     辅助索引：按 ID 排序
用途：sync_timerfd() O(1) 取堆顶            用途：erase_timer() O(log n)
      on_timerfd_read() 收集到期                  回调后存活检查 O(log n)
```

`expireHeap_` 是优先队列（最小堆），只存 `{Timestamp, TimerId}` 作为排序键，O(1) 取最早到期。`timersById_` 是唯一持有 `shared_ptr<Timer>` 的索引——删除时只从 `timersById_` 移除，`expireHeap_` 中的残留条目在 `on_timerfd_read()` 收集阶段通过 `timersById_.find()` 检测后丢弃（懒删除）。这种设计避免了在优先队列中做 O(n) 的随机删除。

### 3. 线程安全模型：one loop per thread

所有索引操作（增删改查）都在 EventLoop 线程执行，**无锁设计**。外部线程通过 `loop_->run_in_loop(lambda)` 将操作投递到 EventLoop 线程：

```cpp
TimerId add_timer(std::function<void()> callback, Timestamp when, milliseconds interval) {
    TimerId id = TimerId(nextTimerId_.fetch_add(1, std::memory_order_relaxed));
    auto timer = std::make_shared<Timer>(id, std::move(callback), when, interval);
    loop_->run_in_loop([this, timer]() {
        // 此 lambda 在 EventLoop 线程执行，无需加锁
        expireHeap_.push({ timer->expiration(), timer->id() });
        timersById_[timer->id()] = timer;
        sync_timerfd();  // 新定时器可能成为最早到期的，需重新武装 timerfd
    });
    return id;
}
```

### 4. 到期处理流程

```
on_timerfd_read()
  ├─ read_timerfd()                  // 消费事件（读 8 字节），避免 LT 模式反复通知
  ├─ 收集到期定时器                    // 循环弹出堆顶
  │    ├─ 堆顶在 timersById_ 中不存在 → pop + continue（懒删除）
  │    ├─ 堆顶 expiration > now      → break（未到期，停止收集）
  │    └─ 堆顶 expiration <= now     → 加入 expiredTimers + pop
  ├─ 逐条执行回调
  │    ├─ timer->run()               // 执行用户回调
  │    ├─ timersById_.find(id)       // 回调后重新检查存活（回调可能取消了自身或其他定时器）
  │    │    └─ 不存在 → continue
  │    ├─ 一次性定时器：timersById_.erase(id)
  │    └─ 重复定时器：timer->restart(now) → expireHeap_.push({newExp, id})
  └─ sync_timerfd()                  // 根据新的最早到期时间重新武装 timerfd
```

关键细节：收集阶段通过 `timersById_.find()` 实现懒删除——`erase_timer` 只从 `timersById_` 移除，`expireHeap_` 中的残留条目在此处被过滤丢弃，避免在优先队列中做 O(n) 随机删除。

### 5. sync_timerfd 的武装/解除逻辑

```cpp
void sync_timerfd() {
    if (expireHeap_.empty()) {
        disarm_timerfd();  // it_value = {0,0}，timerfd 不再触发
    } else {
        reset_timerfd(expireHeap_.top().first);  // 武装到最早到期时间
    }
}

void reset_timerfd(Timestamp expiration) {
    auto duration = expiration - steady_clock::now();  // 时间基准统一在这一处
    newValue.it_value = to_timespec(duration);          // to_timespec 只做格式转换
    ::timerfd_settime(timerFd_, 0, &newValue, nullptr);
}
```

`to_timespec` 只接收相对时间（duration）并做格式转换，不在内部重复取 `steady_clock::now()`，避免两次取时间之间的微小偏差。时间计算集中在 `reset_timerfd` 一处。

每次添加或移除定时器后都必须调用 `sync_timerfd()`，确保 timerfd 始终反映当前最早到期时间——旧定时器被取消而新定时器更晚时，如果忘记解除则会导致空唤醒；新定时器更早时如果忘记重新武装则会延迟触发。

## Result — 效果

- **零线程开销**：定时器完全融入 epoll 事件循环，不需要额外的定时器线程
- **操作复杂度**：添加 O(log n)，取消 O(log n)，获取最早到期 O(1)，批量收集到期 O(k)
- **线程安全**：所有索引操作在 EventLoop 线程无锁执行，外部线程通过 `run_in_loop` 投递
- **可扩展**：框架内所有定时需求（心跳保活、连接超时、延迟任务）共用同一套机制
- **正确性**：`on_timerfd_read` 中每条定时器执行回调后重新检查 `timersById_` 存活状态，避免回调中取消定时器导致悬空访问

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
            ├── TimerId id = TimerId(nextTimerId_++)               # ① 在调用方线程生成唯一 ID，保证同步返回
            ├── std::make_shared<Timer>(id, std::move(callback), when, interval)  # 按值接收 + move，零拷贝
            └── loop_->run_in_loop(lambda)                         # ② 投递到 EventLoop 线程
                    └── [lambda 在 EventLoop 线程执行]
                            ├── expireHeap_.push({expiration, id})  # ③ 插入主索引（优先队列，按时间最小堆）
                            ├── timersById_[id] = timer                 # ③ 插入辅助索引（按 ID 排序）
                            └── sync_timerfd()                          # ④ 武装 timerfd
                                    ├── expireHeap_ 为空? → disarm_timerfd()    # 解除武装
                                    └── 否则 → reset_timerfd(最早到期时间)           # 重设 timerfd
                                            ├── duration = expiration - now()        # 时间基准统一在这一处
                                            └── ::timerfd_settime(timerFd_, ...)     # 系统调用
```

### 1.2 取消定时器（跨线程安全入口）

```
外部线程调用
    └── TimerQueue::erase_timer(timerId)
            └── loop_->run_in_loop(lambda)                      # 投递到 EventLoop 线程
                    └── [lambda 在 EventLoop 线程执行]
                            ├── timersById_.erase(timerId)      # ① 从辅索引移除（持有 shared_ptr 的唯一索引）
                            └── sync_timerfd()                  # ② 最早到期可能已变，重新同步
                                    └── 堆顶若已被 erase 的定时器 → on_timerfd_read 中懒删除过滤
```

注意：`erase_timer` 不从优先队列中删除（O(n) 不可接受），只从 `timersById_` 移除。残留条目在 `on_timerfd_read()` 收集阶段通过 `timersById_.find()` 检测后丢弃（懒删除）。

### 1.3 定时器到期处理管道（EventLoop 线程内，单线程无锁）

```
epoll_wait 返回 timerFd_ 可读事件
    └── Channel::handle_events()                              # Channel 分发就绪事件
            └── read_callback → TimerQueue::on_timerfd_read()  # 管道唯一入口
                    ├── read_timerfd(timerFd_)                  # ① 消费事件（读 8 字节）
                    │       └── ::read(timerFd_, &howmany, 8)   # 必须消费，否则 LT 模式反复通知
                    ├── ② 收集到期定时器（循环弹出堆顶）
                    │       └── while (!expireHeap_.empty()):
                    │               ├── top = expireHeap_.top()
                    │               ├── timersById_ 中不存在 top.id → pop + continue  # 懒删除过滤
                    │               ├── top.expiration > now → break                  # 未到期，停止收集
                    │               └── top.expiration <= now → expiredTimers.push + pop
                    ├── ③ 逐条执行回调
                    │       └── for each timer in expired:
                    │               ├── timer->run()                         # 执行用户回调
                    │               │       └── callback_()                  # 回调内可能取消自身或其它定时器
                    │               ├── timersById_.find(timer->id())        # 回调后重新检查存活
                    │               │       └── 不存在 → continue            # 已被回调取消，跳过
                    │               ├── !timer->is_repeat()?                 # 一次性定时器
                    │               │       └── timersById_.erase(timer->id())  # 清理辅索引
                    │               └── timer->is_repeat()?                 # 重复定时器
                    │                       ├── timer->restart(now)           # 以当前时间为基准推进到期时间
                    │                       └── expireHeap_.push({newExp, id})  # 重新入堆
                    └── sync_timerfd()                          # ④ 根据新的最早到期重新武装
                            ├── expireHeap_ 为空? → disarm_timerfd()
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
              |  +-- expireHeap_ ────> priority_queue              |  │
              |  |    (主索引: 最小堆)     <TimerKey, greater<>>       |  |
              |  |                         ┌─────────────────┐        |  |
              |  |                         | {T+100ms, id=1} │ ← top |  |
              |  |                         | {T+200ms, id=2} │        |  |
              |  |                         | {T+500ms, id=3} │        |  |
              |  |                         └─────────────────┘        |  |
              |  |                         只存排序键，不持有对象       |  |
              |  |                              |                      |  |
              |  |                         懒删除：收集时通过           |  |
              |  |                         timersById_.find 过滤       |  |
              |  +-- timersById_ ────────> map< TimerId,              |  |
              |       (辅索引: 按ID)          shared_ptr<Timer> >      |  |
              |       (唯一持有 Timer)         ┌─────────────┐         |  |
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
            read_timerfd()   懒删除+收集到期     逐条执行+清理
              (消费事件)     (pop + find过滤)   (run + 检查存活)


            数据流向 (add_timer):
            ==================
            调用方线程                    EventLoop 线程
            ─────────                    ──────────────
            TimerId id                     │
                 │                         │
            Timer obj ──run_in_loop──> expireHeap_.push({when, id})
                                        timersById_[id] = timer
                                             │
                                        sync_timerfd()
                                             │
                                    timerfd_settime(timerFd_, expiration - now())


            数据流向 (到期触发):
            ==================
            timerfd 可读 (epoll)
                 │
                 v
            on_timerfd_read()
                 │
         ┌───────┼──────────┐
         v       v          v
      消费    懒删除+收集    逐条执行
     事件    (pop堆顶,      ├─ run()
             find过滤)      ├─ 检查存活 (辅索引)
                            ├─ 一次性: 清理辅索引
                            └─ 重复: restart + push回堆
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

### 3.2 优先队列 + ID 索引的协作模型

```
expireHeap_:  priority_queue<pair<Timestamp, TimerId>>   // 主索引（只存排序键）
timersById_:      map<TimerId, shared_ptr<Timer>>            // 辅索引（唯一持有 Timer 对象）

  expireHeap_ (最小堆)         timersById_ (唯一持有者)
  ┌───────────────────────┐       ┌──────────────────────────┐
  │ {T+100ms, id=1}       │       │ id=1 → shared_ptr<Timer> │──→ Timer(id=1)
  │ {T+200ms, id=2}       │       │ id=2 → shared_ptr<Timer> │──→ Timer(id=2)
  │ {T+500ms, id=3}       │       │ id=3 → shared_ptr<Timer> │──→ Timer(id=3)
  └───────────────────────┘       └──────────────────────────┘
         只存键，不持有对象               引用计数管理对象生命周期
```

`expireHeap_` 只存 `{Timestamp, TimerId}` 排序键，不持有 `shared_ptr`。`timersById_` 是唯一持有 `shared_ptr<Timer>` 的索引。删除时只从 `timersById_` 移除，优先队列中的残留条目通过懒删除过滤。Timer 的生命周期完全由 `timersById_` 的引用计数管理。

### 3.3 各索引的操作复杂度

| 操作 | expireHeap_ | timersById_ | 说明 |
|------|:---:|:---:|------|
| 查找最早到期 | O(1) | O(n) | `top()` 即最早 |
| 按 ID 查找 | 不支持 | O(log n) | 优先队列不支持随机访问 |
| 批量收集到期 | O(k log n) | — | k 次 pop，每次 O(log n) |
| 插入 | O(log n) | O(log n) | push + insert |
| 删除（取消） | 懒删除 O(1) | O(log n) | 只从 timersById_ 移除，堆中残留条目收集时过滤 |

---

## 四、 核心设计要点提炼 (Key Architectural Points)

| 设计点 (Design Pattern / Principle) | 代码体现与说明 |
|---|---|
| **Reactor 集成 (timerfd + epoll)** | `timerfd_create` 将时间事件转化为 fd 可读事件，统一纳入 epoll 多路复用，无需独立定时器线程。`Channel(timerFd_)` 注册读回调 `on_timerfd_read`，完全融入 EventLoop 事件循环 |
| **门面模式 (Facade)** | `TimerQueue` 对外只暴露 `add_timer` / `erase_timer` 两个公开接口，隐藏内部双索引、timerfd 系统调用、线程投递等所有实现细节。调用方（EventLoop、TcpConnection）无需感知 timerfd 的存在 |
| **单一职责原则 (SRP)** | `Timer` 只保存单个定时器的到期时间和回调，不参与调度；`TimerQueue` 只负责编排调度和 timerfd 管理；`Channel` 只负责 fd 事件分发。三层职责清晰分割 |
| **one loop per thread 并发模型** | 所有索引操作限定在 EventLoop 线程执行（`assert_in_loop_thread` 守卫），零锁设计。跨线程访问通过 `loop_->run_in_loop(lambda)` 投递，利用 eventfd 唤醒机制立即执行 |
| **依赖注入 (Dependency Injection)** | `TimerQueue(EventLoop* loop)` 通过构造函数注入所属 EventLoop，不持有所有权（裸指针），符合"生命周期由智能指针管理，访问用裸指针"的项目规范 |
| **优先队列 + 懒删除 (Lazy Deletion)** | `expireHeap_` 使用优先队列（最小堆），O(1) 取最早到期。删除时只从 `timersById_` 移除（唯一持有 `shared_ptr` 的索引），优先队列中的残留条目在 `on_timerfd_read()` 收集阶段通过 `find()` 检测后丢弃，避免 O(n) 随机删除 |
| **回调后存活检查 (Defensive Check)** | `on_timerfd_read` 中每条定时器执行后重新 `timersById_.find(id)`，因为回调内可能通过 `erase_timer()` 移除了其他定时器。这是一种防御式编程，避免访问已释放对象 |
| **RAII 管理 fd 生命周期** | `timerFd_` 由 `TimerQueue` 构造函数创建，`timerChannel_`（`unique_ptr<Channel>`）析构时自动 `::close(fd)`。timerfd 的创建和销毁与 TimerQueue 生命周期严格绑定 |
| **解除武装避免空唤醒** | `sync_timerfd` 中队列为空时调用 `disarm_timerfd` 设置 `it_value={0,0}`，避免 timerfd 在没有定时器时仍然触发，浪费 epoll 唤醒的 CPU 开销 |
| **CLOCK_MONOTONIC 时钟源** | `timerfd_create(CLOCK_MONOTONIC, ...)` 使用单调时钟，不受系统时间调整（如 NTP 校时、管理员手动改时间）影响，保证定时器行为可预测 |

---

## 五、 面试核心要点总结

1. **为什么用 timerfd 而不是独立线程？** — 融入 epoll 事件循环，零额外线程开销，统一的事件处理模型。
2. **为什么用优先队列 + 懒删除而不是两个 map？** — 优先队列 O(1) 取最早到期，但不支持 O(log n) 随机删除。解决方案：`erase_timer` 只从 `timersById_` 移除，优先队列中的残留条目在 `on_timerfd_read()` 收集时通过 `find()` 过滤（懒删除），避免 O(n) 遍历。
3. **如何保证线程安全？** — one loop per thread 模型，所有索引操作在 EventLoop 线程执行，外部线程通过 `run_in_loop` 投递，eventfd 唤醒。
4. **重复定时器如何实现？** — `on_timerfd_read` 中检查 `is_repeat()`，调用 `restart(now)` 推进到期时间，`push` 回优先队列。
5. **回调中取消定时器安全吗？** — 安全。`on_timerfd_read` 在每次 `run()` 后重新检查 `timersById_`，被取消的定时器会被跳过。
6. **timerfd 延迟最小 1ms 是怎么保证的？** — `to_timespec` 中 `if (duration < 1ms) duration = 1ms`，因为 timerfd 不接受 0 延迟。
