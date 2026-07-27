# epoll Reactor：高并发事件循环

Tudou 使用 Linux `epoll` 和 Reactor 模型驱动网络事件：一个 IO 线程等待多个 fd，只在 fd 就绪时执行对应回调。`EpollPoller`、`EventLoop` 与 `Channel` 分别负责底层等待、调度和单 fd 事件分发。

# Situation — 情境

传统的阻塞式服务器常采用“一个连接一个线程”。当大量连接都在等待数据时，大量线程会同时阻塞：线程栈占用内存，上下文切换消耗 CPU，慢连接还会长期占住线程。

高并发服务需要让一个线程同时管理许多连接：没有事件时不消耗 CPU，有事件时只处理真正活跃的 fd。

# Task — 任务

Reactor 层需要完成以下工作：

1. 使用 `epoll` 等待大量 fd 的就绪事件；
2. 将一个 fd 的读、写、关闭、错误事件绑定为明确回调；
3. 让主循环直接体现“等待 → 分发 → 执行任务”的顺序；
4. 分离 epoll 机制与上层调度，避免底层 Poller 调用 TCP、HTTP 等业务逻辑；
5. 保持 fd 注册关系与 `Channel` 生命周期同步。

# Action — 设计与实现

## 三个对象的职责

| 对象 | 负责什么 | 不负责什么 |
| :--- | :--- | :--- |
| `EpollPoller` | `epoll_wait`、`epoll_ctl`、收集就绪 Channel | 调用业务回调、拥有连接 |
| `EventLoop` | 驱动本轮事件与 pending functors 的执行顺序 | TCP、HTTP、RPC 协议 |
| `Channel` | 一个 fd 的事件兴趣和读写/关闭/错误回调 | 关闭 fd、拥有上层对象 |

这种边界让“底层 I/O 机制”和“Reactor 调度策略”保持分离：Poller 只告诉 EventLoop 哪些 fd 就绪，EventLoop 决定何时分发它们。

## 主循环：等待、分发、执行任务

`EventLoop::loop()` 是整个 Reactor 的核心路径：

```cpp
while (!isQuit_) {
    const auto& activeChannels = poller_->poll(pollTimeoutMs_);
    for (Channel* channel : activeChannels) {
        channel->handle_events();
    }

    do_pending_functors();
}
```

它对应的事件流程是：

```text
epoll_wait 返回就绪 fd
  → EpollPoller 写入 Channel::revents_
  → EventLoop 遍历活跃 Channel
  → Channel 分发读、写、关闭、错误回调
  → EventLoop 执行本轮跨线程投递的任务
```

`poll()` 只返回本轮的活跃 `Channel*` 列表，不直接调用 `Channel::handle_events()`。因此，将来若要改变分发顺序或增加批处理策略，只需在 EventLoop 中调整。

## Poller：将 epoll 结果转换为 Channel

`epoll_event.data.ptr` 保存 Channel 指针。`epoll_wait` 返回后，Poller 写入本轮就绪掩码并收集活跃列表：

```cpp
const int numReady = ::epoll_wait(epollFd_.fd(), eventList_.data(),
                                  static_cast<int>(eventList_.size()), timeoutMs);

activeChannels_.clear();
for (int i = 0; i < numReady; ++i) {
    auto* channel = static_cast<Channel*>(eventList_[i].data.ptr);
    channel->set_revents(eventList_[i].events);
    activeChannels_.push_back(channel);
}
```

`activeChannels_` 是 Poller 的复用成员，避免每轮 poll 重新分配容器；`eventList_` 会随就绪事件数量调整容量，避免固定数组在负载变化时成为限制。

## Channel：fd 注册与事件分发

本项目采用 eager Channel 生命周期：

```text
Channel 构造
  → EventLoop::update_channel()
  → epoll_ctl(ADD)

事件兴趣变化
  → epoll_ctl(MOD)

Channel 析构
  → EventLoop::remove_channel()
  → epoll_ctl(DEL)
```

Channel 只管理 fd 在 epoll 中的注册关系；fd 的真正关闭由 `Socket` 等上层 owner 负责。这避免了一个对象同时承担“事件订阅”和“资源所有权”两种职责。

本轮事件的分发顺序为：单独的 `EPOLLHUP` 先走关闭回调；`EPOLLERR` 触发错误回调；随后分别处理读和写。`EPOLLHUP | EPOLLIN` 同时到来时仍先走读回调，让上层通过 `read == 0` 正确识别 EOF。

# Result — 结果

- 一个 EventLoop 可等待和处理多个连接，无事件时阻塞在 `epoll_wait`；
- epoll 只返回就绪 fd，避免逐个轮询全部连接；
- 主循环短而完整，便于理解和定位事件顺序；
- Poller、Loop、Channel 的职责明确，TCP/HTTP/RPC 可复用同一事件基础设施；
- 多个 EventLoop 可以运行在不同 IO 线程上，进一步利用多核。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| I/O 多路复用 | `epoll` 监视大量 fd 的就绪状态，不替应用完成读写。 |
| Reactor | 应用在事件就绪后主动执行读写与业务回调。 |
| 调度边界 | Poller 收集事件，EventLoop 分发事件，Channel 处理单 fd 回调。 |
| 生命周期 | Channel 构造注册、析构注销；上层 owner 关闭 fd。 |
| 性能来源 | 等待连接不占线程；只处理就绪 fd；活跃事件容器可复用。 |

# 面试核心问答总结

## Q1：epoll 为什么适合高并发服务器？

它让一个线程等待多个 fd，内核只返回就绪事件。线程不需要为每个等待连接阻塞，也不需要反复扫描所有连接；真正的读写仍由应用在事件到达后执行。

## Q2：为什么不让 EpollPoller 直接调用 Channel 回调？

Poller 是 epoll 机制的封装，只应表达“哪些 fd 就绪”。事件分发顺序属于 Reactor 的调度策略，应由同时持有 Poller 和任务队列的 EventLoop 决定，这样边界更清晰。

## Q3：Channel 为什么不负责关闭 fd？

Channel 只代表 epoll 中的事件订阅；Socket 才拥有 fd。把两者分开后，连接关闭、析构顺序和资源所有权更明确，Channel 也能用于 eventfd、timerfd、listen fd 等不同 fd。

## Q4：这是否意味着 epoll 自动完成网络读写？

不是。epoll 只通知可读、可写等状态。回调仍需要调用 `read`、`write`，并处理 `EAGAIN`、短读和短写；这正是非阻塞 I/O 文档讨论的内容。
