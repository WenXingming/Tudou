# One Loop Per Thread 设计详解

## 一、核心思想

**One Loop Per Thread** = 每个线程最多拥有一个 EventLoop，每个 EventLoop 终生归属于创建它的那个线程。

这不是"建议"，而是通过 `assert(false)` 强制实施的硬约束：

```cpp
// EventLoop.cpp:18
thread_local EventLoop* EventLoop::loopInthisThread = nullptr;

// 构造函数中（第 45-48 行）
if (loopInthisThread != nullptr) {
    assert(false);  // 同一线程不能创建第二个 EventLoop
}
loopInthisThread = this;
```

这意味着：**每个线程只能有一个 epoll 实例、一个定时器队列、一个待执行任务队列**。所有 I/O 事件、定时器事件、跨线程投递的任务，全部在这个 EventLoop 的 `loop()` 主循环中被串行处理。

## 二、为什么要这样设计

### 2.1 消除数据竞争

多线程编程中最复杂的部分是对共享数据的加锁。One Loop Per Thread 通过**线程隔离**从根本上消除竞争：

| 数据 | 访问线程 | 是否需要锁 |
|------|----------|------------|
| `EpollPoller::channels_` (fd → Channel* 映射) | 仅 loop 线程 | 否 |
| `TimerQueue::timersByExpire_` / `timersById_` | 仅 loop 线程 | 否 |
| `pendingFunctors_` 任务队列 | 生产者可跨线程，消费者仅 loop 线程 | 是 (`mtx_`) |
| `TcpConnection::readBuffer_` / `writeBuffer_` | 仅该连接绑定的 loop 线程 | 否 |
| `TcpServer::connections_` (fd → TcpConnection 映射) | 多 loop 线程并发读写 | 是 (`connectionsMutex_`) |

可以看到，整个框架中**只有两个地方需要锁**：
- `pendingFunctors_` 的跨线程入队（临界区极小，只做 push/swap）
- `connections_` 的并发增删（唯一跨 loop 共享的数据结构）

其余所有数据都是线程私有的，根本不需要考虑并发访问。

### 2.2 简化回调语义

所有 I/O 回调（`on_read`、`on_write`、`on_close`、`on_error`）都在同一个线程执行。业务代码不需要考虑"这个回调可能在哪个线程被调用"，不需要为每个连接单独加锁。一个连接的所有事件从生到死都在同一个 IO 线程上发生。

### 2.3 避免惊群效应

每个 EventLoop 有自己独立的 epoll 实例。多个 IO 线程不会同时在同一个 epoll 上竞争，避免了惊群问题。

## 三、实现机制

### 3.1 线程归属检查

```cpp
// EventLoop.h:175 — 构造时记录的线程 ID
const std::thread::id threadId_;

// 运行时检查
bool is_in_loop_thread() const {
    return threadId_ == std::this_thread::get_id();  // 纯比较，无锁
}

void assert_in_loop_thread() const {
    assert(is_in_loop_thread());  // Debug 下崩溃，Release 下零开销
}
```

### 3.2 跨线程唤醒：eventfd

当其他线程需要让某个 EventLoop "立即醒来做某事"时，不能直接打断 `epoll_wait`，而是通过 eventfd 写入一个字节：

```
线程 A (非 loop 线程)                EventLoop 线程 B
    │                                      │
    │  loop_->run_in_loop(task)            │ 阻塞在 epoll_wait()
    │    └─ queue_in_loop(task)            │
    │         ├─ push 到 pendingFunctors_  │
    │         └─ wakeup()                  │
    │              └─ write(wakeupFd_, 1) ─┼→ wakeupFd_ 变为可读
    │                                      │ epoll_wait 返回
    │                                      │ 处理就绪事件（包括 wakeupChannel_）
    │                                      │ on_read() 消费 eventfd
    │                                      │ do_pending_functors() 执行 task
```

### 3.3 任务投递：两条路径

```cpp
void run_in_loop(const Functor& cb) {
    if (is_in_loop_thread()) {
        cb();                    // 同线程：直接执行（快速路径）
    } else {
        queue_in_loop(cb);       // 跨线程：入队 + 唤醒
    }
}
```

`queue_in_loop` 的唤醒条件值得注意：

```cpp
if (!is_in_loop_thread() || isCallingPendingFunctors_) {
    wakeup();
}
```

两个场景需要唤醒：
1. **跨线程投递**（`!is_in_loop_thread()`）：loop 线程可能正阻塞在 `epoll_wait`，需唤醒
2. **重入投递**（`isCallingPendingFunctors_`）：loop 线程在执行 pending functors 期间又调了 `queue_in_loop`。因为已通过 swap 把任务移到了局部变量，新任务留在 `pendingFunctors_` 中不会被执行，必须唤醒让下一轮 poll 立即返回

### 3.4 主循环结构

```cpp
void EventLoop::loop(int timeoutMs) {
    isQuit_ = false;
    isLooping_ = true;
    while (!isQuit_) {
        poller_->poll(timeoutMs);       // ① 等待 I/O 事件或超时
        do_pending_functors();          // ② 执行所有排队任务
    }
    isLooping_ = false;
}
```

每轮循环只做两件事：**等内核事件 → 执行排队任务**。绝不在此之外做任何业务逻辑。

## 四、如何判断某个函数必须在 loop 线程执行

这是本框架最重要、也最容易出错的判断。以下给出一套可操作的判断标准。

### 4.1 判断标准

一个函数必须在 loop 线程执行，**当且仅当它直接或间接**：

1. **访问了特定 EventLoop 独有的内核资源**
   - 调用 `epoll_ctl`（ADD/MOD/DEL）—— 因为 epoll 实例是线程私有的
   - 调用 `timerfd_settime` —— timerfd 只被该 loop 的 epoll 监听
   - 对已注册到该 loop epoll 的 fd 执行 `close()`

2. **读写了没有锁保护的事件循环私有数据结构**
   - `EpollPoller::channels_`（fd → Channel* 映射，无锁）
   - `TimerQueue::timersByExpire_` / `timersById_`（双索引，无锁）
   - `EventLoop::pendingFunctors_` 的消费端（生产者有锁保护，但消费只在 loop 线程）
   - `pendingFunctors_` 的 `swap()` 操作

3. **创建或销毁了 Channel 对象**
   - Channel 构造时立即通过 `update_in_register()` 调用 `epoll_ctl(ADD)`
   - Channel 析构时通过 `remove_in_register()` 调用 `epoll_ctl(DEL)`
   - 因此 Channel 的生灭必须在 loop 线程

4. **读写了绑定到特定 loop 线程的连接私有数据**
   - `TcpConnection::readBuffer_` / `writeBuffer_`（无锁，仅该连接的 loop 线程访问）
   - `TcpConnection::isClosed_`（无锁，仅该连接的 loop 线程访问）

### 4.2 从调用链推断

更实用的方法是**顺着调用链往底层看**：

```
你的函数
  └→ 调用了 EventLoop::update_channel()    → 必须在 loop 线程
  └→ 调用了 EventLoop::remove_channel()    → 必须在 loop 线程
  └→ 调用了 EventLoop::has_channel()       → 必须在 loop 线程
  └→ 调用了 EventLoop::loop()              → 必须在 loop 线程
  └→ 创建/销毁了 Channel 对象              → 必须在 loop 线程
  └→ 调用了 TimerQueue 的私有方法          → 必须在 loop 线程
  └→ 调用了 run_in_loop / queue_in_loop    → 任意线程均可
  └→ 调用了 quit()                         → 任意线程均可
  └→ 调用了 run_after / run_every / cancel → 任意线程均可（内部用 run_in_loop）
  └→ 调用了 is_in_loop_thread()            → 任意线程均可（只读比较）
```

### 4.3 特殊情况：跨线程安全的函数

有些函数虽然属于 EventLoop，但**可以跨线程调用**，因为它们内部通过 `run_in_loop` 自动投递：

**TimerQueue::add_timer / erase_timer**
```cpp
TimerId TimerQueue::add_timer(callback, when, interval) {
    // ① 在调用方线程生成 ID（原子自增，线程安全）
    TimerId id = TimerId(nextTimerId_++);

    // ② 创建 Timer 对象（shared_ptr，无共享状态）
    auto timer = std::make_shared<Timer>(id, callback, when, interval);

    // ③ 索引操作投递到 loop 线程
    loop_->run_in_loop([this, timer]() {
        timersByExpire_[...] = timer;   // 在 loop 线程操作索引
        timersById_[timer->id()] = timer;
        sync_timerfd();
    });
    return id;
}
```

关键：**把"需要线程归属的操作"通过 run_in_loop 延迟到 loop 线程**，而不是在调用方线程直接执行。

**EventLoop::quit()**
```cpp
void quit() {
    isQuit_ = true;           // atomic 写入，任意线程安全
    if (!is_in_loop_thread()) {
        wakeup();             // 跨线程唤醒，安全（write 系统调用）
    }
}
```

### 4.4 正面例子：变量的线程归属

一个常见的设计疑问：`TcpServer::connections_` 用 `connectionsMutex_` 保护而不是仅靠 loop 线程——为什么？

因为 `connections_` 被**多个不同的 loop 线程**访问：
- **写入**：主线程在 `on_connect` 中通过 `store_connection` 写入
- **读取/删除**：各 IO 线程在 `remove_connection` 中删除

如果 `connections_` 只被一个 loop 线程访问（比如用单线程模式），就不需要锁。这里的锁保护的是**跨 loop 的共享数据**，与 One Loop Per Thread 不矛盾，而是它的补充。

### 4.5 反面例子：忘记线程归属的后果

假设有人在非 loop 线程直接调了 `TcpConnection::send()`：

```cpp
// 线程 X（不是该连接的 loop 线程）
void TcpConnection::send(const std::string& msg) {
    loop_->assert_in_loop_thread();  // Debug 下直接崩溃
    writeBuffer_->write_to_buffer(msg);
    channel_->enable_writing();      // → update_in_register() → epoll_ctl(MOD)
}
```

如果没有 `assert_in_loop_thread`，会导致：
1. `writeBuffer_` 被并发写入（数据竞争，UB）
2. `epoll_ctl` 被并发调用（epoll 实例不是线程安全的）

## 五、线程模型全景

```
  主线程 (main)                       IO 线程 0                       IO 线程 1
  ==============                      =========                      =========

  TcpServer                           EventLoopThread                EventLoopThread
    │                                     │                              │
    ├─ EventLoop (mainLoop_)              ├─ EventLoop (ioLoop)          ├─ EventLoop (ioLoop)
    │   ├─ EpollPoller (epollFd)          │   ├─ EpollPoller             │   ├─ EpollPoller
    │   ├─ wakeupFd_ (eventfd)            │   ├─ wakeupFd_               │   ├─ wakeupFd_
    │   ├─ TimerQueue (timerfd)           │   ├─ TimerQueue              │   ├─ TimerQueue
    │   ├─ pendingFunctors_               │   ├─ pendingFunctors_        │   ├─ pendingFunctors_
    │   │                                 │   │                          │   │
    │   ├─ Acceptor                       │   ├─ TcpConnection(fd=10)    │   ├─ TcpConnection(fd=11)
    │   │   └─ Channel(listenFd)          │   │   ├─ Channel(fd=10)      │   │   ├─ Channel(fd=11)
    │   │                                 │   │   ├─ readBuffer_         │   │   ├─ readBuffer_
    │   │                                 │   │   └─ writeBuffer_        │   │   └─ writeBuffer_
    │   │                                 │   │                          │   │
    │   └─ loop() ← 阻塞于此              │   └─ loop() ← 阻塞于此       │   └─ loop() ← 阻塞于此

  跨线程通信：
  主线程 on_connect ──run_in_loop──→ IO线程0 创建 TcpConnection
  IO线程0 发现超时 ──connectionsMutex_──→ 从 connections_ 移除
  IO线程0 需要退出 ──quit() + wakeup()──→ IO线程0 的 loop() 返回
```

## 六、设计总结

| 原则 | 说明 |
|------|------|
| **一线程一循环** | 一个线程最多一个 EventLoop，通过 `thread_local` 强制保证 |
| **数据随线程隔离** | epoll 实例、定时器索引、连接缓冲区都是线程私有的，无需加锁 |
| **跨线程靠投递** | 需要跨线程操作时，不直接访问数据，而是把任务投递到目标线程执行 |
| **锁只在边界** | 只有当数据真的被多个 loop 线程共享（如 `connections_`）时才加锁 |
| **断言即文档** | `assert_in_loop_thread()` 不仅是安全检查，更是对调用方的契约声明 |
