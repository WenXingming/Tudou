# OneLoopPerThread 设计：线程归属、无锁编程与跨线程唤醒

OneLoopPerThread 的核心不是“系统只有一个线程”，而是“每个 EventLoop 的私有状态只有一个线程可以直接访问”。线程之间通过任务投递和唤醒协作。

# Situation — 项目背景

一个 `EventLoop` 通常拥有：

- epoll 实例；
- eventfd 唤醒 fd；
- timerfd 和 TimerQueue；
- pending functors；
- 归属于该 loop 的 Channel 和连接。

如果多个线程可以直接修改这些状态，就必须在 epoll、定时器、连接 buffer 和 Channel 事件之间大量加锁，正确性和性能都会变差。

因此需要明确：

```text
主线程：accept 新连接
IO 线程：拥有连接并处理 I/O
其他线程：通过任务投递影响目标 loop
```

# Task — 设计目标与约束

1. 一个 EventLoop 只有一个归属线程；
2. 线程私有状态在热路径上无锁访问；
3. 跨线程操作统一通过 `run_in_loop()` / `queue_in_loop()`；
4. 目标 loop 阻塞等待时，投递方可以立即唤醒它；
5. 错误线程访问尽早通过断言或检查暴露。

# Action — 设计方案与实现

### EventLoop 的线程归属

EventLoop 使用线程局部指针记录当前线程已经拥有的 loop：

```cpp
thread_local EventLoop* loopInThisThread = nullptr;

EventLoop::EventLoop(...) {
    assert(loopInThisThread == nullptr);
    loopInThisThread = this;
}
```

这样可以在构造阶段发现“一个线程创建多个 EventLoop”的错误。

### 线程内串行访问

连接、Channel、TimerQueue 和 Poller 都绑定到所属 loop。典型路径是：

```text
owner EventLoop
  → Channel 回调
  → TcpConnection 读写
  → 修改 readBuffer_ / writeBuffer_
  → 更新 Channel 关注事件
```

这些操作不需要额外互斥锁，因为同一条连接的事件始终在 owner loop 线程执行。

### 跨线程任务投递

目标 loop 已经在当前线程时直接执行，否则把任务放入 pending functors：

```cpp
void EventLoop::run_in_loop(Functor callback) {
    if (is_in_loop_thread()) {
        callback();
        return;
    }
    queue_in_loop(std::move(callback));
}
```

`queue_in_loop()` 写入任务队列后调用 `wakeup()`，向 eventfd 写入数据：

```text
其他线程 queue_in_loop
  → pendingFunctors_ 加入任务
  → eventfd 写入唤醒值
  → epoll_wait 返回
  → EventLoop 处理 wakeup fd
  → 执行 pending functors
```

如果 EventLoop 正在执行 pending functors，新的任务也会触发唤醒，避免任务被延迟到下一轮。

### EventLoop 主循环

主流程应能直接读出 Reactor 的调度顺序：

```cpp
while (!quit_) {
    auto activeChannels = poller_->poll(timeoutMs);
    for (Channel* channel : activeChannels) {
        channel->handle_events();
    }
    do_pending_functors();
}
```

### OneLoopPerThread 的边界

“无锁”只适用于线程独占状态，不能扩大为“整个服务器完全线程安全”：

- pending functors 队列仍需要 mutex；
- stop、状态标志等跨线程状态通常使用 atomic；
- 多线程共享配置必须在启动前完成，或提供明确同步；
- 外部线程不能直接访问连接的 buffer 或 Channel。

# Result — 结果

- 每个连接有稳定的线程语义；
- EventLoop、TimerQueue 和 Channel 的热路径可以无锁串行执行；
- 跨线程操作有统一入口和唤醒机制；
- 错误线程访问可以尽早暴露；
- 多个 IO 线程之间并行，但每个 loop 内部保持简单的单线程模型。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 唯一归属 | 一个 EventLoop 只属于一个线程。 |
| 线程私有 | 连接、Channel、TimerQueue 在 owner loop 内直接访问。 |
| 跨线程协作 | 通过 pending functors 和 eventfd 唤醒目标 loop。 |
| 无锁边界 | 只对线程独占状态无锁，共享队列仍需同步。 |
| 主循环 | `poll → dispatch → pending functors`。 |

# 面试核心问答总结

## Q1：OneLoopPerThread 是不是单线程模型？

不是。它是多线程模型：多个 IO 线程并行，每个 EventLoop 内部串行处理自己的连接和事件。

## Q2：为什么连接内部通常不需要锁？

因为连接固定归属一个 EventLoop，读写 buffer 和 Channel 状态只在 owner loop 线程修改。

## Q3：跨线程如何让目标 EventLoop 执行任务？

把任务放入 pending functors，再通过 eventfd 唤醒目标 loop；目标线程在事件循环中执行任务。

## Q4：为什么说不是完全无锁？

跨线程任务队列、状态标志和真正共享的数据仍然需要 mutex 或 atomic。无锁只表示线程私有热路径不需要锁。
