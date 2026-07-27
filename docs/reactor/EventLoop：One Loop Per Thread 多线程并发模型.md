# One Loop Per Thread：多线程并发模型

Tudou 用 One Loop Per Thread 将连接的可变状态绑定到单个 IO 线程。它不是“整个服务器无锁”，而是通过线程归属减少共享数据，让连接读写的热路径无需竞争锁。

# Situation — 情境

一个 `EventLoop` 持有 epoll、eventfd、TimerQueue、pending functors，以及归属它的 Channel 和连接。若任意线程都能直接修改这些状态，就必须在 Buffer、Channel 事件、定时器和连接表之间频繁加锁；即使没有死锁，也很难证明事件顺序正确。

网络库既要利用多核，也要避免把每条连接变成多个线程同时操作的共享对象。

# Task — 任务

线程模型需要保证：

1. 一个 EventLoop 只归属一个线程；
2. 一条连接的 Channel、Buffer 和定时器操作只在 owner loop 中执行；
3. 新连接可以被分配到多个 IO 线程；
4. 其他线程可以安全地请求目标 loop 执行任务；
5. 目标 loop 阻塞在 `epoll_wait` 时，跨线程任务不能被延迟到超时后才执行。

# Action — 设计与实现

## 一个线程只能有一个 EventLoop

`EventLoop` 在构造时记录所属线程，并通过线程局部指针阻止同一线程创建第二个 loop：

```cpp
thread_local EventLoop* EventLoop::loopInThisThread = nullptr;

EventLoop::EventLoop(int pollTimeoutMs)
    : threadId_(std::this_thread::get_id()) {
    assert(loopInThisThread == nullptr);
    loopInThisThread = this;
}

bool EventLoop::is_in_loop_thread() const {
    return threadId_ == std::this_thread::get_id();
}
```

这不是为了限制线程数，而是明确状态归属：一个线程可以有一个 main loop 或一个 IO loop；多个线程则各自拥有不同 loop。

## 线程池分配连接

`EventLoopThreadPool` 在当前线程创建 main loop，并创建若干 `EventLoopThread` 作为 IO 线程。新连接由 main loop 轮询选择一个 IO loop：

```text
main loop 接收连接
  → get_next_loop()
  → round-robin 选择 IO loop
  → 在目标 loop 创建 TcpConnection
  → 后续读写始终由该 IO loop 处理
```

`EventLoopThread` 构造时会等待后台线程完成 loop 初始化并发布指针：

```cpp
thread_ = std::thread(&EventLoopThread::thread_func, this);

std::unique_lock<std::mutex> lock(loopMutex_);
loopCondition_.wait(lock, [this] { return loop_ != nullptr; });
```

因此线程池拿到的 EventLoop 已经是可运行对象，而不是尚未初始化的裸指针。

## 跨线程任务：投递而不是直接修改

调用方不在 owner loop 时，`run_in_loop()` 转为任务投递：

```cpp
void EventLoop::run_in_loop(const Functor& cb) {
    if (is_in_loop_thread()) {
        cb();
        return;
    }
    queue_in_loop(cb);
}

void EventLoop::queue_in_loop(const Functor& cb) {
    {
        std::lock_guard<std::mutex> lock(pendingFunctorsMutex_);
        pendingFunctors_.push(cb);
    }
    if (!is_in_loop_thread() || isCallingPendingFunctors_) {
        wakeup();
    }
}
```

任务队列是唯一需要由多个线程共同写入的结构，因此它受 mutex 保护。回调执行前，EventLoop 先把队列交换到局部变量，再在无锁状态下顺序执行：

```cpp
FunctorQueue functors;
{
    std::lock_guard<std::mutex> lock(pendingFunctorsMutex_);
    functors.swap(pendingFunctors_);
}
while (!functors.empty()) {
    Functor functor = std::move(functors.front());
    functors.pop();
    functor();
}
```

## eventfd：及时唤醒目标 loop

跨线程投递后，目标 loop 可能正阻塞在 `epoll_wait`。`wakeup()` 向非阻塞 eventfd 写入计数值；eventfd 本身由一个 Channel 监听，因此 epoll 立即返回：

```text
其他线程 queue_in_loop(task)
  → pending functors 入队
  → write(eventfd)
  → epoll_wait 返回 wakeup 事件
  → EventLoop::on_read() 消费 eventfd
  → do_pending_functors() 执行 task
```

当 EventLoop 正在执行 pending functors 时又有新任务到来，也会唤醒下一轮，避免新任务被延后到下一次 poll 超时。

## 无锁边界

| 状态 | 访问方式 |
| :--- | :--- |
| TcpConnection 的 Buffer、Channel、定时器 | 仅 owner loop 线程访问，无需 mutex |
| pending functors | 多线程投递，使用 mutex |
| `isQuit_`、`isLooping_` | 跨线程状态，使用 atomic |
| 服务器配置和回调 | 启动前设置；运行期不应无同步修改 |

# Result — 结果

- 多个 IO 线程可以并行处理不同连接；
- 单条连接的状态在线程内串行变化，读写热路径没有锁竞争；
- 跨线程操作只有一个入口：投递到目标 loop；
- eventfd 避免跨线程任务因 poll 阻塞而延迟；
- 锁的范围收缩到真正共享的任务队列，心智模型更小。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 线程归属 | 一个 EventLoop 绑定一个线程，一条连接绑定一个 IO loop。 |
| 负载分配 | main loop 轮询选择 IO loop，多个线程并行处理连接。 |
| 跨线程协作 | 不直接修改目标对象，统一投递 Functor。 |
| 及时唤醒 | eventfd 使阻塞的 epoll_wait 立即返回。 |
| 无锁边界 | 线程私有状态无锁；真正共享的队列仍加锁。 |

# 面试核心问答总结

## Q1：One Loop Per Thread 是单线程模型吗？

不是。它是多线程模型：多个 IO 线程并行运行不同 EventLoop；每个 loop 内部串行处理自己负责的连接。

## Q2：为什么连接内部通常不需要锁？

连接被固定分配给一个 owner loop，读写 Buffer、Channel 事件和定时器只在这个线程修改。跨线程调用不直接碰这些状态，而是投递任务。

## Q3：为什么投递任务后还需要 eventfd？

目标 loop 可能在 `epoll_wait` 中等待，单纯把任务塞进队列无法让它立刻检查队列。写 eventfd 会产生一个可读事件，唤醒 epoll。

## Q4：为什么不能说整个服务器“无锁”？

pending functors 是多线程共享队列，仍需要 mutex；退出标志等共享状态也需要 atomic。无锁只适用于严格线程独占的连接热路径。
