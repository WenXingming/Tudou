# Binary RPC 单连接多路复用

单连接多路复用允许多个 RPC 请求共用一条 TCP 连接。请求发送后不必按顺序等待响应，客户端通过 `sequenceId` 把响应交给正确的调用者。

# Situation — 情境

如果每次调用都独占连接，请求会被串行化；如果每个并发调用创建新连接，又会增加 fd、端口、握手和心跳开销。

需要在连接数量有限的情况下，让多个请求同时处于等待状态，并支持响应乱序返回。

# Task — 任务

设计需要解决：

1. 多个请求共享一条连接；
2. 请求和响应能够一一匹配；
3. 多线程发送时不能交叉写入一个 RPC 帧；
4. 只有一个接收线程也能唤醒多个调用者；
5. 连接断开时，所有未完成请求都能得到失败结果。

# Action — 设计与实现

### 发送：先登记，再发送

```cpp
void BinaryRpcChannel::CallMethod(/* ... */) {
    uint64_t seq = nextSequenceId_++;
    auto context = std::make_shared<ResponseContext>();
    auto future = context->promise.get_future();

    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        pendingRequests_[seq] = context;
    }

    sendRequest(encode_request(method, request, seq));
    future.get();
}
```

必须先写入 `pendingRequests_`，再发送请求。否则响应可能先到达，接收线程找不到对应的调用上下文。

### 响应：按 sequenceId 分发

多个请求可以这样发送：

```text
request(seq=1) → request(seq=2) → request(seq=3)
```

响应可以乱序返回：

```text
response(seq=2) → 唤醒调用者 2
response(seq=1) → 唤醒调用者 1
```

接收线程解码完整帧后，从 `pendingRequests_` 取出对应上下文，写入响应并唤醒等待者：

```cpp
auto it = pendingRequests_.find(header.sequenceId);
if (it == pendingRequests_.end()) {
    return; // 迟到或未知响应
}

auto context = it->second;
pendingRequests_.erase(it);
context->response->ParseFromString(body);
context->promise.set_value();
```

### 两把锁解决不同问题

- `sendMutex_`：保证一个 RPC 帧的字节不会被其他线程插入；
- `mapMutex_`：保护 `sequenceId -> ResponseContext` 映射。

它们分别保护字节流和请求状态，不能混为一谈。

### 一个接收线程

一个连接只需要一个接收线程负责读取和拆帧：

```text
接收线程：读取 TCP 字节流 → 解码完整响应 → 按 seq 查找上下文
调用者：发送请求后等待 future 或挂起协程
```

“一个接收线程”不等于“请求串行”。请求处理和调用者唤醒仍然可以并发。

### 线程模式与协程模式

两种模式共享相同的 `sequenceId -> context` 设计：

| 模式 | 等待方式 | 收到响应后 |
| :--- | :--- | :--- |
| 线程模式 | `future.get()` 阻塞线程 | `promise.set_value()` |
| 协程模式 | `yield()` 挂起协程 | `resume()` 恢复对应协程 |

协程改变的是等待方式，不是多路复用本身。

### 连接断开

连接断开时必须一次性处理所有 pending 请求：

```text
连接断开
  → 取出并清空 pendingRequests_
  → 给每个上下文设置异常结果
  → 唤醒线程或恢复协程
```

否则调用者会永久等待。

# Result — 结果

- 多个 RPC 请求可以共用一条 TCP 连接；
- 响应可以乱序返回，并通过 `sequenceId` 准确匹配；
- 单接收线程负责拆帧，多个调用者独立等待；
- 线程模式和协程模式共享同一套请求映射；
- 连接断开时所有未完成调用都会被唤醒并失败返回。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| `sequenceId` | 请求和响应之间的唯一匹配键。 |
| `pendingRequests_` | 保存 seq 到调用上下文的映射。 |
| `sendMutex_` | 防止多个线程交叉写入 RPC 帧。 |
| 单接收线程 | 统一读取和解码，不代表请求串行。 |
| 断开清理 | 连接断开时结束所有未完成请求。 |

# 面试核心问答总结

## Q1：单连接多路复用解决什么问题？

它让多个请求共享一条 TCP 连接并同时等待响应，减少连接和握手开销。

## Q2：为什么需要 sequenceId？

响应可能乱序返回，必须通过 ID 找到正确的调用上下文，不能依赖响应到达顺序。

## Q3：为什么只有一个接收线程仍然可以并发？

接收线程只负责读取、拆帧和分发响应；不同调用者由各自的 future 或协程独立等待。

## Q4：为什么发送前要先登记 pendingRequests_？

防止响应先到达时查不到上下文，导致调用永久等待。
