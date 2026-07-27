# Channel tie：事件回调的生命周期安全

`Channel::tie_to_object()` 用 `weak_ptr` 保护事件回调期间的对象生命周期。它解决的不是普通析构问题，而是“当前回调尚未返回，owner 却在回调中被移除并析构”的悬空指针问题。

# Situation — 情境

`TcpConnection` 由 `shared_ptr` 管理，`Channel` 是它的成员；但 EventLoop 从 epoll 得到的是裸 `Channel*`。关闭事件的回调可能移除服务器连接表中的最后一个 `shared_ptr<TcpConnection>`：

```text
EventLoop 遍历活跃 Channel*
  → Channel::handle_events()
  → TcpConnection::close_connection()
  → TcpServer 从连接表 erase TcpConnectionPtr
  → TcpConnection 析构，成员 Channel 同时析构
  → Channel::handle_events() 仍在执行
```

若后续代码继续访问 Channel 的成员或回调，便会产生 use-after-free。

# Task — 任务

生命周期设计需要同时满足：

1. owner 在整轮事件回调结束前不能析构；
2. Channel 平时不能长期拥有 TcpConnection，避免改变正常连接回收；
3. Channel 不应依赖 `TcpConnection` 的具体类型；
4. 无 owner 的 Channel，例如 Acceptor 的 listen fd，仍可正常使用；
5. tie 必须在对象已由 `shared_ptr` 管理后建立。

# Action — 设计与实现

## 建立弱绑定

`TcpConnection` 通过工厂函数先创建 `shared_ptr`，再绑定并开启读事件：

```cpp
std::shared_ptr<TcpConnection> conn(
    new TcpConnection(loop, std::move(connSocket), peerAddr));

conn->channel_.tie_to_object(conn);
conn->channel_.enable_reading();
return conn;
```

顺序不能颠倒：构造函数中对象尚未被 `shared_ptr` 接管，不能安全调用 `shared_from_this()`；读事件必须在 tie 建立后再开启，避免尚未完成保活设置就触发回调。

Channel 内部保存的是类型擦除后的弱引用：

```cpp
std::weak_ptr<void> tie_;
bool isTied_;
```

`weak_ptr` 不增加连接引用计数，`void` 让 Channel 不需要知道 owner 是 TcpConnection 还是其他上层对象。

## 回调开始前临时保活

`handle_events()` 在每轮分发前尝试升级弱引用：

```cpp
void Channel::handle_events() {
    if (!isTied_) {
        handle_events_with_guard();
        return;
    }

    std::shared_ptr<void> guard = tie_.lock();
    if (guard) {
        handle_events_with_guard();
    }
}
```

`guard` 是栈上局部变量，生命周期覆盖整个 `handle_events_with_guard()`。即使读、写或关闭回调从 TcpServer 连接表移除了 owner，局部 `shared_ptr` 仍会将析构延后到本轮事件函数返回。

若 `lock()` 失败，说明 owner 在本轮开始前已经销毁；Channel 不再执行回调，避免访问失效对象。

## 为什么活跃列表中的裸指针仍可控

`EpollPoller` 返回的是 `Channel*` 列表，但 EventLoop 对这个列表的遍历发生在同一个 owner 线程。外部线程不能直接销毁连接，只能投递到该 loop；因此，poll 到分发之间不会被并发析构打断。

真正的风险来自“分发中的回调自己触发连接移除”，这正是 tie 的保护范围。tie 不替代线程归属模型，两者共同保证安全。

## 为什么不用长期 shared_ptr

如果 Channel 永久持有 `shared_ptr<TcpConnection>`：

1. Channel 会参与连接的长期所有权；
2. 连接表移除后对象可能仍被 Channel 保留，回收时机不清晰；
3. 底层 Reactor 组件会与上层 TCP 类型形成强耦合。

tie 只在回调期间生成临时强引用，准确覆盖风险窗口，不改变正常析构路径。

# Result — 结果

- 回调期间 TcpConnection 与其成员 Channel 不会提前析构；
- owner 已失效时可安全跳过本轮事件；
- Channel 保持通用，可用于 TcpConnection、Acceptor、eventfd、timerfd 等不同场景；
- 没有引入长期强引用和循环引用；
- 生命周期安全与 One Loop Per Thread 的线程安全边界保持清晰。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 风险窗口 | 事件回调正在执行时，关闭回调可能释放 owner。 |
| 平时引用 | `weak_ptr<void>`，不拥有上层对象且不依赖具体类型。 |
| 回调引用 | `lock()` 成临时 `shared_ptr<void>`，只覆盖一轮分发。 |
| 建立时机 | `shared_ptr` 接管 TcpConnection 后、开启读事件前。 |
| 协作前提 | EventLoop 线程归属防止 poll 与分发之间被跨线程析构打断。 |

# 面试核心问答总结

## Q1：tie 解决的是什么问题？

它防止事件回调执行期间，关闭流程移除最后一个 `shared_ptr<TcpConnection>`，从而让当前 Channel 或回调中的 `this` 变成悬空指针。

## Q2：为什么使用 weak_ptr，而不是让 Channel 长期保存 shared_ptr？

Channel 只需要在回调期间保活 owner。长期 shared_ptr 会混淆所有权、延迟连接回收，并让底层 Reactor 依赖上层 TCP 类型；weak_ptr 加临时 lock 正好覆盖风险窗口。

## Q3：为什么是 weak_ptr<void>？

`weak_ptr` 表示不拥有，`void` 做类型擦除。Channel 只关心“回调期间 owner 是否还活着”，不关心 owner 的具体类型。

## Q4：tie 能解决所有并发析构问题吗？

不能。tie 只保护一轮事件回调；跨线程析构问题由 One Loop Per Thread 约束解决：外部线程必须投递任务到 owner loop，不能直接修改或销毁连接。
