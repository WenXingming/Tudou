# BinaryRpc - CoroutineChannel 设计：单连接多路复用与协程恢复

`binary::CoroutineChannel` 是二进制 RPC 唯一的客户端。它使用非阻塞 Socket 和 EventLoop 完成网络收发，通过 `sequenceId` 在一条 TCP 长连接上关联并发请求与乱序响应，并在响应到达后恢复正确的有栈协程。

# Situation — 情境

一个 EventLoop 中可以运行多个协程。它们可能先后通过同一个 Channel 发出 RPC 请求，而服务端完成请求的顺序并不固定。客户端既不能阻塞 EventLoop，也不能依赖响应到达顺序。

# Task — 任务

客户端需要同时解决：非阻塞建连与收发、TCP 半包和粘包、请求与响应关联、协程挂起和恢复，以及连接失败时唤醒全部等待者。

# Action — 设计

## pending call 直接表达等待关系

```cpp
struct PendingCall {
    google::protobuf::Message* response;
    std::shared_ptr<Coroutine> coroutine;
    std::string* error;
};

std::unordered_map<uint64_t, PendingCall> pendingCalls_;
```

每个 `sequenceId` 对应一个等待中的调用：响应应该写入哪个 Protobuf 对象、完成后恢复哪个协程，以及错误应该写回哪里。`response` 和 `error` 指向挂起的有栈协程栈，`coroutine` 保证这块栈在响应到达前存活。PendingCall 直接按值保存在 map 中，不需要单独堆分配，也不会形成“协程栈反向持有 Coroutine”的自引用环。

## 调用主流程

```text
CallMethod
  -> 创建 sequenceId 和 PendingCall
  -> FrameCodec 编码请求
  -> 写入 outputBuffer_
  -> Coroutine::yield()

EPOLLIN
  -> Connection 拆出完整响应帧
  -> sequenceId 查找 PendingCall
  -> 解析 Protobuf response
  -> 投递 Coroutine::resume()
```

`yield()` 只挂起当前用户态协程，EventLoop 所在线程仍可继续处理其他连接和协程。恢复动作通过 `queue_in_loop()` 延后执行，避免在 Socket 读回调内部重入业务调用栈。

## 为什么无需 mutex

Channel 的构造、调用、读写事件、pending map 和关闭都发生在同一个 EventLoop 线程。线程归属由断言和 Reactor 使用约束保证，因此 `pendingCalls_`、`nextSequenceId_` 和 `open_` 都是普通成员，不需要 `mutex` 或 `atomic`。

## 连接失败

连接关闭或协议错误时，Channel 为每个 PendingCall 写入错误，并把对应协程全部投递回 EventLoop。协程从 `yield()` 后恢复并抛出异常，因此不会永久挂起。

# Result — 结果

- 一条 TCP 连接支持多个协程并发等待和响应乱序匹配；
- 网络等待不阻塞 EventLoop 线程；
- 删除阻塞 Socket、后台接收线程、Future、发送锁和通用回调层；
- pending call 的数据、完成和失败流程都能在一个类中直接读懂。

# 权衡：如果需要阻塞客户端

阻塞客户端可以在上层单独封装：调用前创建 `promise/future`，接收线程按 `sequenceId` 设置结果，调用线程执行 `future.get()`。这种实现会引入后台接收线程、发送锁和跨线程 pending map；当前项目没有这个需求，因此不保留对应生产代码。

# 面试表达

> 二进制 RPC 客户端使用 nonblocking Socket、epoll 和 Boost.Coroutine2。调用时把 sequenceId、响应对象和当前协程放入 pending map，然后 yield；响应到达后按 sequenceId 找回上下文，在原 EventLoop 中恢复协程。所有状态由一个 Loop 独占，所以不需要 mutex。为了保持实现简洁，我删除了收益有限的阻塞客户端和通用 Multiplexer，直接用 PendingCall 表达真实等待关系。
