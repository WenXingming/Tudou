# TimerQueue 设计与实现（基于 Linux timerfd 和 std::set）

`TimerQueue` 将“到期时间管理”抽象为底层的 `timerfd` 读就绪事件，并借由 `EventLoop` 的多路复用机制，实现了定时任务与网络 I/O 任务的单线程扁平化调度。

本设计基于 **Thread-Per-Core** 的无锁化多线程并发模型，并将底层的定时器容器由优先队列（懒删除）重构为了 `std::set`（物理即时删除），配合 `std::shared_ptr` 解决复杂的定时器生命周期管理问题。

---

# 一、 TimerQueue 设计与实现（STAR 法则）

## 1. Situation — 项目背景

Tudou 是一个基于 Reactor 模式的多线程 C++ 网络框架。在实现 TCP 连接的心跳机制时，需要定时器基础设施支持——连接的保活探测、空闲超时断开等功能都依赖定时器。框架需要在**不引入额外定时线程**的前提下，提供高效、并发安全且无锁的定时器管理能力，且必须与现有的 epoll 事件循环无缝集成。

## 2. Task — 任务目标

设计并实现一个定时器队列组件，满足以下约束：
1. **零额外线程**：定时器到期检测复用 EventLoop 的 epoll 轮询，不占用独立线程。
2. **多线程并发安全**：允许任意外部线程并发添加/取消定时器，但实际操作和回调执行必须在 EventLoop 线程内单线程串行化完成。
3. **消除无效空唤醒**：在定时器被取消时，必须即时解除内核武装，杜绝残留失效定时器导致的网络库空唤醒与 CPU 损耗。
4. **生命周期防御**：防止在批量定时器到期时，因回调函数内部发生“自我取消”或“相互取消”而导致的野指针（Use-After-Free）崩溃。

---

## 3. Action — 技术方案与实现

### 3.1 底层事件源选型：Linux timerfd
使用 `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)` 创建定时器文件描述符。它将“时间流逝”映射为“文件描述符可读”，这使得定时器事件可以像普通的 socket 读写事件一样被注册到 `epoll` 中进行统一监听。

* **优势**：
  * **单线程复用**：避免了 `std::thread` + `sleep` 带来的线程上下文切换和同步开销。
  * **精度高且稳定**：使用 `CLOCK_MONOTONIC` 单调时钟，不受系统校时（如 NTP、管理员修改时间）的影响。

---

### 3.2 双索引数据结构（std::set + std::map）
原方案使用 `std::priority_queue` 配合懒删除，无法物理上即时取消堆中的定时器，导致 `timerfd` 空唤醒和无法在无定时器时解除武装。重构方案采用 **`std::set` + `std::map`** 的双索引设计：

```
      expireSet_                                 timersById_
std::set<TimerEntry>                   std::map<TimerId, std::shared_ptr<Timer>>
  (按到期时间排序)                            (按 TimerId 排序)
        │                                          │
        ▼                                          ▼
用于 O(1) 获取最早到期元素，                     用于 O(log n) 按 ID 检索，
并在 erase 时实现 O(log n) 物理即时删除           以及执行时的生命周期存活校验
```

* **`TimerEntry` 结构**：
  定义为 `std::pair<Timestamp, std::shared_ptr<Timer>>`。`std::set` 默认先比较第一个元素（到期时间 `Timestamp`），在时间相同时比对 `shared_ptr` 的内存地址作为 Tie-breaker，保证元素唯一性并实现自动升序。
* **物理即时删除（Immediate Deletion）**：
  取消定时器时，通过 `timersById_` 找到该定时器的 `shared_ptr<Timer>` 并获取其到期时间，随后在 $O(\log N)$ 时间内直接在 `expireSet_` 中将其抹去。由于失效的定时器被当场彻底清除，`sync_timerfd()` 获取的永远是真实的、最早到期的活动定时器时间，空唤醒率降为 **0**。

---

### 3.3 多线程并发设计（One-Loop-Per-Thread 无锁化）
为了避免在 `TimerQueue` 的 `add_timer` / `erase_timer` 等高频操作中引入互斥锁（Mutex）和条件变量，网络库严格遵守 **One-Loop-Per-Thread** 线程模型：

* **线程隔离边界**：
  `TimerQueue` 的内部数据结构（`expireSet_` 和 `timersById_`）是非线程安全的。**我们规定：只有拥有该 TimerQueue 的 EventLoop 线程，才有权对其进行读写修改。**
* **跨线程任务投递**：
  当外部线程（如工作线程池）需要添加或取消定时器时，不允许直接调用修改接口，而是必须通过 [EventLoop::run_in_loop](file:///home/wxm/Tudou/src/tudou/reactor/EventLoop.cpp#L102) 将操作封装为 Lambda 任务，投递到主线程的等待任务队列中：

```cpp
TimerId TimerQueue::add_timer(std::function<void()> callback, Timestamp when, std::chrono::milliseconds interval) {
    TimerId id = TimerId(nextTimerId_.fetch_add(1, std::memory_order_relaxed));
    auto timer = std::make_shared<Timer>(id, std::move(callback), when, interval);

    // 线程安全：通过 run_in_loop 将索引修改操作强制排队回 EventLoop 主线程执行
    loop_->run_in_loop(
        [this, timer]() {
            expireSet_.insert({ timer->get_expiration(), timer });
            timersById_[timer->get_id()] = timer;
            sync_timerfd(); // 重新校准 timerfd 到期时间
        }
    );
    return id;
}
```

* **同步机制**：如果投递时主线程正阻塞在 `epoll_wait` 中，投递动作会通过向 `eventfd` 写入 8 字节数据将其唤醒，使主线程能立即处理添加/取消任务。这种架构实现了**外部并发访问安全，主线程内部零锁运行**的高性能设计。

---

### 3.4 生命周期防御：`shared_ptr` 防止“相互取消”崩溃
定时器回调函数的执行充满不确定性。例如：定时器 A 到期，触发的回调函数中执行了 `cancel(B)`；而定时器 B 也在本次到期批次中。此时，极易发生内存悬空野指针崩溃。

#### 1. 致命缺陷场景（如果不用 `shared_ptr`）
如果 `TimerQueue` 仅存储 `Timer*` 裸指针或由 `std::unique_ptr` 强拥有：
1. **收集阶段**：定时器 A 和 B 在同一时刻到期，它们被存入待执行临时数组：`expiredTimers = {A*, B*}`。
2. **执行阶段**：
   1. 首先轮到 A 执行：调用 `A->run()`。
   2. 在 A 的回调内，业务逻辑调用了 `erase_timer(B)`，B 从底层 Map 中被移除并**立即被释放析构**。
   3. 轮到 B 执行：调用 `B->run()`。但此时 `B*` 指向的内存已成野指针，**程序立即发生 Use-After-Free 崩溃**。

#### 2. `shared_ptr` 护栏机制的实现
重构设计利用 `std::shared_ptr` 的引用计数实现**延迟析构保活**：

```cpp
void TimerQueue::on_timerfd_read() {
    read_timerfd(timerFd_.fd());
    const Timestamp now = std::chrono::steady_clock::now();
    
    // 1. 临时容器通过 shared_ptr 共享所有权，增加引用计数
    std::vector<std::shared_ptr<Timer>> expiredTimers;
    
    while (!expireSet_.empty()) {
        auto it = expireSet_.begin();
        if (it->first > now) break;
        expiredTimers.push_back(it->second); // 计数 +1，防 UAF
        expireSet_.erase(it); // 从 set 移出
    }

    // 2. 顺序执行回调
    for (const auto& timer : expiredTimers) {
        // 3. 执行前存活检查：即使 timer 在前面其他定时器的回调中被 cancel 掉了，
        // 它的内存依然被 expiredTimers 数组安全保活，但我们必须在这里拦截它，防止其执行。
        if (timersById_.find(timer->get_id()) == timersById_.end()) {
            continue; // 已被取消，安全跳过，绝不执行
        }

        timer->run();

        // 4. 执行后再次确认：防止定时器在其自身的回调中注销了自己（Self-Cancellation）。
        const auto stillExists = timersById_.find(timer->get_id());
        if (stillExists == timersById_.end()) {
            continue; 
        }

        if (!timer->is_repeat()) {
            // 一次性定时器：执行完后从索引中彻底删除
            timersById_.erase(timer->get_id());
            continue;
        }

        // 5. 重复定时器重调度
        const Timestamp t = std::chrono::steady_clock::now();
        timer->reschedule(t);
        expireSet_.insert({ timer->get_expiration(), timer });
    }

    sync_timerfd();
}
```

> [!IMPORTANT]
> 引用计数为定时器提供了安全的执行生命周期。被执行前取消的定时器不会被提前释放，而是会完好地留在 `expiredTimers` 数组中，直到 `on_timerfd_read()` 退出、临时数组析构时，才会被安全销毁。

---

### 3.5 sync_timerfd 的精确重武装
因为改用了 `std::set` 物理删除，`expireSet_` 的头部永远保持着最新的、绝对有效的最早到期时间戳。`sync_timerfd` 实现变得极为简练，**不再需要由于懒删除而存在的“堆顶循环清理”过滤逻辑**，也不再会发生因懒删除条目积压导致无法进入 `disarm` 的问题：

```cpp
void TimerQueue::sync_timerfd() {
    // 1. 若集合为空，说明当前无任何活动定时器，立即解除内核武装，杜绝 CPU 空转
    if (expireSet_.empty()) {
        disarm_timerfd(); 
        return;
    }
    // 2. 以 std::set 头部最早到期的真实定时器时间重新武装 timerfd
    reset_timerfd(expireSet_.begin()->first);
}
```

---

## 4. Result — 重构效果

* **物理即时删除**：取消定时器操作从原先的“仅在 Map 删除 + 堆中等待懒弹出”变为了**物理当场擦除**。
* **零无效唤醒**：`sync_timerfd()` 同步数据 100% 准确。在无定时器运行时，`timerfd` 能即时被 `disarm`，系统空唤醒率降为 0。
* **极高性能的无锁并发**：外层通过 `run_in_loop` 进行排队，内层操作在 EventLoop 线程无锁串行，吞吐量极大。
* **坚固的生命周期防线**：基于 `std::shared_ptr` 的引用计数保护机制，使得网络库即便面对复杂的、自我取消、相互取消的回调交互时，也能维持 100% 的内存安全。

---

# 二、 深度源码级分析与流转

## 1. 架构与数据流转图 (Architecture & Data Flow Diagram)

```
                         外部线程 (可多个)                       EventLoop 线程 (唯一)
                         ──────────────                        ─────────────────────
                               │                                          │
               add_timer()     │     erase_timer()                        │
               ┌───────┴───────┴───────┐                                  │
               │  run_in_loop(lambda)  │  # 跨线程排队                    │
               └───────────┬───────────┘                                  │
                           │                                              │
                           v                                              │
               +-------------------------------------------------------+  │
               |                   TimerQueue                          |  │
               |                                                       |  │
               |  +-- timerFd_ ──────────> epoll 监听                  |  │
               |  +-- timerChannel_ ──────> 回调 → on_timerfd_read()   |  │
               |                                                       |  │
               |  +-- expireSet_ ────> std::set<TimerEntry>            |  │
               |  |    (主索引: 物理排序)   ┌─────────────────┐        |  |
               |  |                         | {T+100ms, ptrA} | ← begin|  |
               |  |                         | {T+200ms, ptrB} |        |  |
               |  |                         | {T+500ms, ptrC} |        |  |
               |  |                         └─────────────────┘        |  |
               |  |                         实时升序，支持 O(log n) 删除 |  |
               |  |                              │                      |  |
               |  |                              ▼                      |  |
               |  +-- timersById_ ────────> map< TimerId,              |  |
               |       (辅索引: 按ID)          shared_ptr<Timer> >      |  |
               |       (唯一持有 Timer)         ┌─────────────┐         |  |
               |                               | id=1: TimerA |         |  |
               |                               | id=2: TimerB |         |  |
               |                               | id=3: TimerC |         |  |
               |                               └─────────────┘         |  |
               +-------------------------------------+-----------------+  │
                                                     |                    │
                           timerfd 到期 (epoll 通知)  |                    │
                                                     v                    │
                               on_timerfd_read() ─────────────────────────┘
                                       │
                    ┌─────────────────┼─────────────────┐
                    │                 │                 │
                    v                 v                 v
             read_timerfd()      收集到期(即时)     逐条执行+存活检查
              (消费 8 字节)      (erase从expireSet) (shared_ptr临时保活)
```

---

## 2. 各索引操作复杂度对比

在引入 `std::set` 物理即时删除重构后，操作复杂度的改变如下：

| 操作 | 优先队列 + 懒删除 (旧) | std::set + 即时删除 (新) | 重构效果与差异说明 |
| :--- | :---: | :---: | :--- |
| **查找最早到期** | $O(1)$ | $O(1)$ | 均为常数级。旧方案读 `top()`，新方案读 `*begin()`。 |
| **按 ID 查找** | 不支持 | 不支持 | 均通过 `timersById_` ($O(\log N)$) 辅助进行。 |
| **批量到期弹出** | $O(K \log N)$ | $O(K \log N)$ | 耗时一致。 |
| **添加定时器** | $O(\log N)$ | $O(\log N)$ | 均需要对两个容器进行插入动作。 |
| **取消定时器** | $O(\log N)$ (懒删除仅删Map) | **$O(\log N)$ (两端物理擦除)** | **核心区别**：新方案增加了一次 `std::set::erase` 操作，仍为对数复杂度，但换来了 timerfd 绝对精准的武装，消除了无效空唤醒的开销。 |

---

# 三、 核心设计要点提炼 (Key Architectural Points)

| 设计点 (Design Pattern) | 代码体现与说明 |
| :--- | :--- |
| **Reactor 物理集成** | `timerfd` 使时间事件退化为常规的 I/O 可读事件，借用 `Channel` 的多路复用直接融入 `EventLoop` 主循环，免去多线程轮询开销。 |
| **one loop per thread** | 通过 `loop_->run_in_loop(lambda)` 把所有修改和回调限定在 EventLoop 线程内单线程串行化，**规避了在核心数据结构上加锁的开销**。 |
| **物理即时删除（std::set）** | 取消定时器时利用 `timersById_` 获取到期时间，并在 `expireSet_` 中同步擦除。没有脏条目残留，使得 `sync_timerfd` 能瞬间解武装，避免 CPU 空转。 |
| **生命周期护栏（shared_ptr）** | 收集到期定时器时将 `std::shared_ptr<Timer>` 存入临时数组。即使回调在执行期被取消，其引用计数也保持 $\ge 1$，完美隔绝了 Use-After-Free 隐患。 |
| **单调时钟源 (Monotonic)** | 使用 `CLOCK_MONOTONIC` 创建 `timerfd`，确保即便系统遭遇校时突变，定时任务的绝对间隔精度也绝不受影响。 |

---

# 四、 面试核心问答总结 (Q&A)

#### Q1：为什么重构掉 `priority_queue` 懒删除，改用 `std::set`？
优先队列（堆）不支持对中间元素的随机删除（删除只能是 $O(N)$），导致取消定时器时只能做“懒删除”（即只在 Map 中抹去，让废弃节点残留在堆中）。这会导致：
1. **空唤醒**：已取消的定时器在到期时，仍会从内核唤醒 `EventLoop`，造成不必要的 CPU 损耗。
2. **武装滞留**：如果当前全部定时器都被取消了，因为废弃元素仍然堆积在堆中，导致 `sync_timerfd()` 无法将其解除武装（disarm），使得 `timerfd` 频繁产生多余唤醒。
改用 `std::set`（底层是红黑树）后，我们可以通过对数复杂度 $O(\log N)$ 物理上直接将元素擦除，实现精准、即时的定时器解除武装，空唤醒率归零。

#### Q2：既然都在同一个线程执行，为什么还会有“相互取消导致的野指针”问题？
虽然是单线程串行执行回调，但**回调函数的执行是嵌套在 TimerQueue 的执行循环内部 of **。
当我们一次性触发了 $5$ 个到期定时器，TimerQueue 必须先把这 $5$ 个定时器放入一个临时列表（如 `expiredTimers`），然后循环遍历执行它们。如果我们在执行第 $1$ 个定时器的回调时，它在代码里调用了 `erase_timer(第 2 个)`。
如果生命周期没有防护，第 2 个定时器对象会在第 1 个回调执行时就被彻底析构释放。当执行循环走到第 2 个定时器并调用 `timer->run()` 时，该指针已指向已被释放的内存，导致 Use-After-Free 致命崩溃。

#### Q3：`std::shared_ptr` 是如何具体解决这个崩溃的？
在收集阶段，临时数组 `expiredTimers` 存储的是 `std::shared_ptr<Timer>`。这会将这批即将触发的定时器引用计数全部加 1。
即使第 1 个定时器的回调注销了第 2 个定时器，导致其从 `timersById_` map 中被删除，由于 `expiredTimers` 数组依然抓着第 2 个定时器的 `shared_ptr`，其对象在内存中并不会被销毁。
在第 2 个定时器准备执行前，我们通过 `timersById_.find(id)` 进行一次快速的存活检查，发现它已被注销，因此安全跳过。当 `on_timerfd_read()` 运行结束离开作用域、`expiredTimers` 被自动析构时，被注销的第 2 个定时器才会被真正、安全地释放。
