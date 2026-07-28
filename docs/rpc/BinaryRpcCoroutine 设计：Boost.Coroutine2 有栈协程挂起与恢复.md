# BinaryRpcCoroutine 设计：Boost.Coroutine2 有栈协程挂起与恢复

`binary::Coroutine` 只封装 Boost.Coroutine2 执行上下文和所属 EventLoop；网络收发与请求状态留在 `CoroutineChannel` 和 `Multiplexer`。

# Situation — 情境

Reactor 客户端不能在 EventLoop 线程内用 `future.get()` 阻塞，否则同一线程无法继续处理 socket 就绪事件。纯回调又会把一次 RPC 的业务流程拆散。

# Task — 任务

调用代码希望保持同步写法，但等待响应时必须把执行权交还 EventLoop；响应到达后还要在原 EventLoop 恢复正确协程。

# Action — 设计

## pull/push 非对称协程

`resume()` 进入协程栈，`yield()` 返回调用者栈。构造 `pull_type` 时 Boost 会立即进入协程函数，因此构造 lambda 首先执行一次 `yield`；等外部 `shared_ptr` 已经建立后，第一次 `resume()` 才运行用户函数。这保证 `shared_from_this()` 的前提成立。

## 当前协程标识

```cpp
static thread_local Coroutine* currentCoroutine_;
```

`CoroutineChannel::CallMethod()` 通过 `Coroutine::current()` 找到当前执行上下文，并验证其 EventLoop 与 Channel 相同。thread_local 只表达“当前线程正在运行哪个协程”，协程对象本身仍由 shared_ptr 管理。

进入、挂起和恢复都保存并还原旧指针；异常路径也会还原，避免后续代码误认为仍运行在已经失败的协程中。

## 生命周期

发起调用时，completion 持有当前协程的 shared_ptr。响应到达后，它通过 `queue_in_loop()` 安排 `resume()`，因此挂起期间协程不会被提前析构，恢复操作也保持 EventLoop 线程归属。

# Result — 结果

- EventLoop 等待 RPC 时不阻塞 OS 线程；
- 业务代码仍可按“调用—获得响应—继续处理”的顺序书写；
- 协程生命周期跨越网络等待期间保持安全；
- Coroutine 不依赖 Socket、Frame 或 Protobuf，职责足够小。

# 面试表达

> 有栈协程保存调用栈，让业务以同步形式表达；真正等待网络时 yield 回 EventLoop。响应回调按 sequenceId 找到请求，再在原 Loop 恢复协程。构造时先挂起一次，是为了等 shared_ptr 所有权建立后再运行用户函数。
