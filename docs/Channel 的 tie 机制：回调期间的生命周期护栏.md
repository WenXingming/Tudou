# Channel tie 机制：回调期间的生命周期护栏

`Channel::tie()` 解决的是一个具体问题：事件回调执行期间，上层 owner 可能被移出连接表并析构，导致当前 `Channel` 成为悬空对象。tie 不长期拥有 owner，只在本轮事件分发期间临时保活。

# Situation — 背景与风险

`TcpConnection` 由 `shared_ptr` 管理，`Channel` 是它的成员，`EventLoop` 通过 `Channel` 分发 fd 事件。

一次关闭回调可能形成这样的调用链：

```text
Channel::handle_events
  → TcpConnection::close_connection
  → TcpServer::remove_connection
  → 连接表 erase
  → TcpConnection / Channel 引用计数归零
  → 当前仍在执行的 Channel 成为悬空对象
```

危险不在于对象最终析构，而在于 `Channel::handle_events()` 尚未返回时，`Channel` 可能已经随着 `TcpConnection` 一起析构。

# Task — 设计目标

1. 事件分发期间 owner 必须保持存活；
2. 平时不能让 `Channel` 长期持有 owner；
3. `Channel` 不依赖 `TcpConnection` 的具体类型；
4. tie 必须在 owner 已经被 `shared_ptr` 管理后建立；
5. 没有 owner 的 `Channel`（例如 Acceptor）仍可正常工作。

# Action — 设计与实现

### 弱绑定，分发时临时保活

`Channel` 保存类型擦除后的弱引用：

```cpp
std::weak_ptr<void> tie_;
bool isTied_;
```

事件分发时执行：

```text
isTied_ == false
  → 直接分发事件

isTied_ == true
  → tie_.lock()
  → 成功：临时 shared_ptr 保活，再分发事件
  → 失败：owner 已销毁，跳过本轮事件
```

典型实现如下：

```cpp
void Channel::handle_events() {
    std::shared_ptr<void> guard;
    if (isTied_) {
        guard = tie_.lock();
        if (!guard) {
            return;
        }
    }

    handle_events_with_guard();
}
```

`guard` 的生命周期覆盖整个 `handle_events_with_guard()` 调用，函数返回后立即释放。

### 为什么使用 weak_ptr<void>

- `weak_ptr` 不增加 owner 的长期引用，避免循环引用和析构延迟；
- `void` 让 Channel 不需要知道 owner 是 `TcpConnection` 还是其他类型；
- `isTied_` 区分普通 Channel 和绑定 owner 的 Channel。

### tie 的建立时机

`shared_from_this()` 只有对象已经被 `shared_ptr` 接管后才有效。因此流程必须是：

```text
创建 TcpConnection
  → shared_ptr 接管对象
  → TcpConnection::activate 中建立 Channel tie
  → 注册并开始处理事件
```

不能在构造函数中提前调用 `shared_from_this()`。

### 为什么不直接保存 shared_ptr

如果 Channel 永久保存 `shared_ptr<TcpConnection>`：

1. 底层 Channel 会依赖上层具体类型；
2. Channel 会参与 owner 的长期生命周期管理；
3. 连接表移除后对象可能仍被 Channel 持有，影响正常析构。

tie 只需要“回调期间保活”，不需要“永久拥有”。

# Result — 结果

- 事件回调期间 owner 不会提前析构；
- owner 被提前销毁时，Channel 可以安全跳过事件；
- Channel 保持通用，不依赖 `TcpConnection` 类型；
- 不引入长期强引用，也不会改变正常连接回收路径；
- Acceptor 等没有 owner 的 Channel 仍走普通分发路径。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 保护时机 | 只在事件分发期间临时保活。 |
| 引用类型 | 平时 `weak_ptr`，进入回调后 `lock()` 成临时 `shared_ptr`。 |
| 类型隔离 | `weak_ptr<void>` 避免 Channel 依赖上层具体类型。 |
| 建立时机 | owner 被 `shared_ptr` 管理后，在 activate 阶段建立 tie。 |
| 无 owner 场景 | 通过 `isTied_` 区分普通 Channel。 |

# 面试核心问答总结

## Q1：tie 解决的是什么问题？

它防止事件回调执行期间 owner 被析构，导致当前 `Channel` 或回调中的 `this` 变成悬空指针。

## Q2：为什么不用 shared_ptr 长期持有？

长期持有会破坏分层并可能形成强引用链。tie 只需要在本轮事件分发期间临时保活。

## Q3：为什么使用 weak_ptr<void>？

`weak_ptr` 避免长期拥有，`void` 让 Channel 不依赖 owner 的具体类型。

## Q4：为什么不能在构造函数里 tie？

构造函数执行时对象可能还没有被 `shared_ptr` 接管，调用 `shared_from_this()` 不安全；应在 activate 阶段建立。
