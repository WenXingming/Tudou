# 事件分发职责上移：将 dispatch_events 从 EpollPoller 移至 EventLoop（STAR 法则）

## Situation — 项目背景

Tudou 是一个基于 Reactor 模式的多线程 C++ 网络框架。`EventLoop` 是 Reactor 的核心调度器，`EpollPoller` 是 epoll 的底层封装。在原实现中，`EpollPoller::poll()` 内部完成了完整的四步流程：等待就绪事件 → 翻译为 Channel 列表 → **分发事件到 Channel** → 调节缓冲区容量。

分发行为（调用 `Channel::handle_events()`）被内聚在 `EpollPoller` 中，导致 `EpollPoller` 跨越了其职责边界。

## Task — 任务目标

将事件分发职责从 `EpollPoller` 上移到 `EventLoop`，使 `EpollPoller` 专注于 epoll 机制的封装（创建、注册、等待、翻译、容量调节），`EventLoop` 负责 Reactor 策略行为（获取就绪列表 → 分发 → 执行待处理任务）。

## Action — 技术方案与实现

### 接口变更

`EpollPoller::poll()` 的签名从 `void` 改为返回 `std::vector<Channel*>`，由 `EventLoop::loop()` 负责遍历并调用 `channel->handle_events()`：

```cpp
// EventLoop::loop() — 分发逻辑回归 EventLoop
auto activeChannels = poller_->poll(timeoutMs);
for (Channel* channel : activeChannels) {
    channel->handle_events();
}
do_pending_functors();
```

### 职责对比

| 层 | 重构前 | 重构后 |
|---|---|---|
| `EpollPoller` | wait + translate + **dispatch** + resize | wait + translate + resize |
| `EventLoop` | 调用 `poller_->poll()` | poll 获取列表 + **dispatch** + do_pending_functors |

### 设计依据

1. **单一职责**：`EpollPoller` 是 I/O 复用**机制**的抽象，不应知道 Channel 有 `handle_events()` 这一上层接口细节。分发就绪事件是 Reactor 的**策略**行为，属于 `EventLoop` 的调度职责。
2. **相邻类通信原则**：`EventLoop` 直接持有 `EpollPoller` 和 `Channel` 两类对象的引用（通过 epoll 注册/注销接口间接管理 Channel），由它来完成"从 Poller 拿到就绪列表 → 通知 Channel"的桥接，不破坏分层。
3. **可读性**：`EventLoop::loop()` 的主循环现在显式展示了 Reactor 模式的完整流程——拿到就绪事件、分发、执行排队任务。读者不需要深入 `EpollPoller::poll()` 内部才能理解事件分发的发生位置。

## Result — 效果

- `EpollPoller` 的公开接口 `poll()` 不再有分发副作用，返回值明确表达"我为你收集了这些就绪 Channel"
- `EventLoop::loop()` 成为 Reactor 流程的唯一可读入口：poll → dispatch → pending functors
- `EpollPoller` 不再依赖 `Channel::handle_events()` 接口，降低了底层组件对上层接口的耦合
- 不影响性能：返回 `vector<Channel*>` 经 NRVO 优化，无额外拷贝开销

---

## 扩展思考：分层的边界在哪里？

软件分层中最常见的误区是"下层为上层做太多事"。`EpollPoller` 分发事件看起来方便了调用方（一行 `poller_->poll()` 就完事），但代价是：

1. **测试困难**：要单独测试事件分发逻辑，必须穿透 Poller
2. **替换困难**：如果要换一种分发策略（如优先级分发、批量分发），必须修改 Poller
3. **理解困难**：读者在 `EventLoop::loop()` 中看到 `poller_->poll()` 无法直观感知事件分发这一步的存在

分层原则：**下层提供能力，上层决定如何使用**。EpollPoller 提供"收集就绪 Channel"的能力，EventLoop 决定"立即顺序分发"。这是正确的切分。
