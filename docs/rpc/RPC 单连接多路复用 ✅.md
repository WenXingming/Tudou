# Binary RPC 单连接多路复用：帧协议、异步响应匹配与协程等待

简历描述：

> 基于帧协议实现单连接请求多路复用及异步响应匹配，基于 Boost.Coroutine2 实现有栈协程，提高客户端并发 RPC 调用的性能。

这项设计解决的是：**如何让多个 RPC 调用同时复用一条 TCP 长连接，并在等待网络响应时不阻塞 EventLoop 线程。**

三个核心技术分别解决不同问题：

```text
帧协议      → 从 TCP 字节流中识别一条完整消息
sequenceId  → 将响应交给正确的请求
有栈协程    → 等待响应时挂起调用现场，而不是阻塞 OS 线程
```

# Situation — 为什么需要单连接多路复用

客户端需要同时发起多个 RPC 请求。最直观的实现会在连接数、线程数或等待时间之间产生明显浪费。

## 方案一：多线程、多连接

一种直接做法是为每个并发调用创建线程和 TCP 连接：

```text
线程 1 → TCP 连接 1 → RPC 请求 1
线程 2 → TCP 连接 2 → RPC 请求 2
线程 3 → TCP 连接 3 → RPC 请求 3
```

它容易理解，但并发量上升后会产生：

- 线程栈、内核调度和上下文切换开销；
- 更多 fd、本地临时端口和服务端连接对象；
- 重复的 TCP 建连、连接维护和心跳成本；
- 大量线程实际只是在等待网络 RTT。

## 方案二：单连接同步调用

为了减少连接，可以让所有请求复用一条连接，但采用最简单的同步 Ping-Pong：

```text
send(request 1)
read(response 1)
send(request 2)
read(response 2)
```

这种模型一次只能存在一个未完成请求。在请求 1 的响应到达前，请求 2 无法发送。若单次服务耗时和网络往返时间为 `T`，顺序执行 `N` 个请求大约需要：

```text
总耗时 ≈ N × T
```

CPU 在大部分等待时间内没有处理新的 RPC，TCP 连接也没有被充分利用。

## 方案三：sequenceId + promise/future

进一步可以先连续发送多个请求，并为每个请求创建结果占位符：

```cpp
const uint64_t sequenceId = nextSequenceId_++;
auto promise = std::make_shared<std::promise<Response>>();
auto future = promise->get_future();
pendingCalls_[sequenceId] = promise;

send_request(sequenceId, request);
```

接收方解析响应后，根据 `sequenceId` 找到对应的 promise：

```cpp
pendingCalls_[response.sequenceId]->set_value(response);
```

调用方在真正需要结果时执行：

```cpp
Response response = future.get();
```

该方案已经实现了“请求连续发送”和“响应异步匹配”，但 `future.get()` 在结果未就绪时仍会阻塞当前 OS 线程。

需要特别说明：**promise/future 只解决结果传递，不负责网络事件调度。** 如果同一个线程既要执行 `future.get()`，又要负责读取 Socket 并设置 promise，它会在响应到达前阻塞，无法继续收包，最终形成死锁。可运行的 Future 方案通常还需要：

- **一个后台接收线程**负责 `read + promise.set_value()`；或者
- 让另一个 EventLoop 线程负责网络 I/O。

这样又引入了线程、互斥锁和调度成本，因此它不是 Tudou 当前保留的实现。

## 方案四：EventLoop + 有栈协程

Tudou 最终采用单个 EventLoop 线程承载多个有栈协程：

```text
协程 1：发送请求 1 → yield
协程 2：发送请求 2 → yield
协程 3：发送请求 3 → yield

EventLoop：epoll 等待网络事件
```

`yield()` 只挂起当前协程，不阻塞 EventLoop 所在的 OS 线程。因此线程仍然可以：

- 启动其他协程中的 RPC 调用；
- 刷新 Socket 发送缓冲区；
- 接收并解析响应；
- 根据 `sequenceId` 恢复正确的协程。

# Task — 设计必须满足什么

当前客户端需要保证：

1. 多个 RPC 请求可以同时复用一条 TCP 长连接；
2. TCP 半包、粘包不会破坏消息边界；
3. 响应不依赖到达顺序，能够匹配原请求；
4. 等待响应时不能阻塞 EventLoop；
5. 同一 EventLoop 内不引入无意义的 mutex；
6. 连接断开时，所有挂起协程都必须恢复并得到错误。

# Action — 当前实现

## 1. 帧协议：解决 TCP 没有消息边界的问题

TCP 只提供连续字节流。一次 `read()` 可能只读到半个 RPC 请求，也可能同时读到多个请求，因此不能把一次读取当成一条消息。

Tudou 的逻辑帧由三部分组成：

```text
┌──────────────────── 20 字节 FrameHeader ────────────────────┐
│ magic │ version │ type │ sequenceId │ headLength │ bodyLength │
└───────────────────────────────────────────────────────────────┘
┌──────────────── CallHead ────────────────┐
│ service_name │ method_name               │
└───────────────────────────────────────────┘
┌──────────────── Protobuf Body ───────────┐
│ 请求参数或响应结果                       │
└───────────────────────────────────────────┘
```

其中：

- `headLength + bodyLength` 确定完整帧边界；
- `type` 区分请求帧与响应帧；
- `sequenceId` 关联一次请求和对应响应；
- `CallHead` 只在请求中携带服务名和方法名；
- `body` 保存序列化后的 Protobuf 消息。

因此，**长度字段解决“这一帧在哪里结束”，sequenceId 解决“这一帧属于谁”**，二者不能互相替代。

`Connection` 在连接级 Buffer 中保留半包，并循环调用 `FrameCodec` 提取所有完整帧，所以同时解决：

```text
拆包：当前字节不足一帧 → 保留，等待下次读取
粘包：当前字节包含多帧 → 循环提取，逐帧处理
```

## 2. PendingCall：记录谁在等待响应

`CoroutineChannel` 为每个未完成请求保存：

```cpp
struct PendingCall {
    google::protobuf::Message* response;
    std::shared_ptr<Coroutine> coroutine;
    std::string* error;
};

std::unordered_map<uint64_t, PendingCall> pendingCalls_;
```

映射关系为：

```text
sequenceId → response + coroutine + error
```

- `response`：响应正文应该反序列化到哪个 Protobuf 对象；
- `coroutine`：响应完成后应该恢复哪个协程；
- `error`：指向协程栈上的错误字符串，连接或协议失败时将错误带回挂起调用点。

`response` 和 `error` 都指向挂起的有栈协程栈；`coroutine` 保证这块栈在响应到达前存活。PendingCall 直接按值保存在 map 中，不需要额外堆分配，也不会形成协程自引用环。当前实现没有再引入通用 Completion 回调或独立 Multiplexer 类。

## 3. 发送：先登记，再进入发送缓冲区

一次调用的关键步骤是：

```cpp
std::string error;
const uint64_t sequenceId = nextSequenceId_++;
pendingCalls_.emplace(
    sequenceId,
    PendingCall{response, coroutine->shared_from_this(), &error});

outputBuffer_.write_to_buffer(
    encode_request(method, request, sequenceId));
channel_->enable_writing();

coroutine->yield();
```

必须先登记 `PendingCall`，再发送请求。否则在极端情况下响应已经返回，客户端却还没有建立 `sequenceId` 映射，会把合法响应误判为未知响应。

请求字节先追加到 `outputBuffer_`，再由非阻塞 Socket 在 `EPOLLOUT` 就绪时继续发送。所有调用都发生在同一个 EventLoop 线程，因此多个完整帧按顺序写入 Buffer，不需要发送锁。

## 4. 等待：yield 挂起协程，不阻塞线程

`Coroutine::yield()` 通过 Boost.Coroutine2 保存当前有栈协程的执行上下文，包括：

- 当前指令位置；
- 栈指针和寄存器；
- `CallMethod()` 及其上层业务函数的局部变量和调用链。

控制权随后返回 EventLoop。业务代码看起来仍然是同步调用：

```cpp
EchoResponse response;
stub.Echo(nullptr, &request, &response, nullptr);
use(response);
```

但 `stub.Echo()` 等待网络期间，底层实际已经 `yield()`，EventLoop 线程没有被阻塞。

## 5. 接收：根据 sequenceId 异步匹配响应

响应到达后的流程是：

```text
EPOLLIN
  → 非阻塞 read，读尽本轮可用字节
  → Connection 累积字节并提取完整 Frame
  → 校验 FrameType::Response
  → pendingCalls_.find(sequenceId)
  → Protobuf ParseFromString(body)
  → 从 pendingCalls_ 删除该调用
  → queue_in_loop(resume)
```

例如客户端依次发送：

```text
request(seq=1)
request(seq=2)
request(seq=3)
```

即使响应按照以下顺序到达：

```text
response(seq=2)
response(seq=3)
response(seq=1)
```

客户端也会分别恢复协程 2、3、1，而不是错误地按发送顺序分配结果。

当前服务端使用同步 Protobuf Service，同一连接上的响应通常仍按请求处理顺序产生；但客户端协议和匹配逻辑不依赖这个顺序，因此能够正确处理乱序响应。

恢复动作使用 `queue_in_loop()`，而不是在 Socket 读回调中直接调用 `resume()`。这样能先结束当前 I/O 回调，再进入业务协程，避免回调栈内部重入业务代码。

## 6. 为什么 pending map 不需要 mutex

以下操作都严格发生在 `CoroutineChannel` 所属的同一个 EventLoop 线程：

```text
生成 sequenceId
写入和删除 pendingCalls_
追加 outputBuffer_
处理 EPOLLIN / EPOLLOUT
恢复协程
关闭连接
```

因此 `pendingCalls_` 是 EventLoop 线程独占状态，不存在并发读写。它不需要 `mutex`，`nextSequenceId_` 和 `open_` 也不需要 `atomic`。

这里的无锁安全来自**线程归属约束**，不是因为 `unordered_map` 本身线程安全。

## 7. 连接失败：恢复所有挂起协程

连接关闭、读写失败或收到非法响应帧后，客户端不能直接清空 pending map，否则协程会永远停在 `yield()`。

当前处理流程是：

```text
标记 Channel 已关闭
  → 停止监听 Socket 事件
  → 为所有 PendingCall 写入错误
  → 将每个 coroutine.resume() 投递回 EventLoop
  → 清空 pendingCalls_
```

协程恢复后从 `yield()` 下一行继续执行，并把保存的错误抛给调用方。

# 完整时序

```text
业务协程              CoroutineChannel           EventLoop / Socket          Server
   │                         │                           │                       │
   │ CallMethod()            │                           │                       │
   ├────────────────────────>│                           │                       │
   │                         │ 分配 sequenceId           │                       │
   │                         │ 登记 PendingCall          │                       │
   │                         │ 编码 Frame                │                       │
   │                         │ 写入 outputBuffer         │                       │
   │ yield                   │                           │                       │
   │<────────────────────────┤                           │                       │
   │                         │──── 等待 EPOLLOUT ───────>│──── request Frame ───>│
   │                         │                           │                       │
   │                         │<───── EPOLLIN ────────────│<── response Frame ────│
   │                         │ 解帧并按 sequenceId 匹配   │                       │
   │                         │ 解析 Protobuf response    │                       │
   │                         │ queue_in_loop(resume) ───>│                       │
   │ resume                  │                           │                       │
   │<────────────────────────┴───────────────────────────│                       │
   │ 从 yield 后继续执行                                 │                       │
```

# Result — 收益

## 连接层面

- 多个并发 RPC 调用复用一条 TCP 长连接；
- 减少 fd、临时端口、建连和连接维护成本；
- 帧协议正确处理 TCP 半包与粘包。

## 并发层面

- 多个请求可以同时处于等待状态，不再采用串行 Ping-Pong；
- `sequenceId` 支持请求与响应准确匹配；
- 客户端不依赖响应顺序。

## 调度层面

- `yield()` 不阻塞 EventLoop 的 OS 线程；
- 不需要为每个请求或每个连接创建接收线程；
- 同一 EventLoop 内无需 pending-map mutex 和发送锁；
- 业务层仍然保持同步、顺序的调用写法。

这里的“提高性能”主要指减少网络等待期间的线程阻塞、线程调度和多连接资源开销，并用较少线程承载更多并发等待中的 RPC；具体 QPS 收益仍应以压力测试结果为准。

# 方案对比


| 方案                 | 连接数量   | 等待方式                | 是否允许多个未完成请求 | 主要问题                             |
| :--------------------- | :----------- | :------------------------ | :----------------------- | :------------------------------------- |
| 多线程、多连接       | 多         | 阻塞 read               | 是                     | 线程和连接开销大                     |
| 单连接同步 Ping-Pong | 一条       | 阻塞 read               | 否                     | RTT 等待导致请求串行                 |
| promise/future       | 可复用一条 | `future.get()` 阻塞线程 | 是                     | 仍需接收执行线程，存在调度和加锁成本 |
| EventLoop + 有栈协程 | 一条       | `yield()` 挂起协程      | 是                     | 需要维护协程生命周期和 Loop 线程约束 |

# 边界与权衡

- 单连接减少资源开销，但仍然存在 TCP 层队头阻塞；超大响应可能影响同连接上的其他调用；
- 单个 EventLoop 线程不适合执行长时间 CPU 密集型业务，否则其他协程也无法获得执行机会；
- 当前客户端没有实现单次 RPC 超时和主动取消，连接级错误会统一恢复全部等待协程；
- 当前服务端是同步 Service 模型，客户端已经具备乱序匹配能力，但服务端异步执行不属于当前实现；
- 当单连接带宽或队头阻塞成为瓶颈时，可以在更上层引入少量连接池，而不是退回“一请求一连接”。

# 面试核心问答

## Q1：什么是单连接多路复用？

多个 RPC 请求同时共享一条 TCP 连接。客户端允许多个请求处于未完成状态，并通过帧头中的 `sequenceId` 将每个响应匹配回正确的调用上下文。

## Q2：帧协议和 sequenceId 分别解决什么？

长度字段解决 TCP 字节流中的消息边界，处理半包和粘包；`sequenceId` 解决请求与响应身份匹配。只有长度字段只能拆出消息，不知道响应属于哪个请求。

## Q3：为什么不能只用单连接同步调用？

同步 Ping-Pong 在收到前一个响应前不能发送下一个请求，吞吐受每次服务耗时和 RTT 累加影响，连接的大量时间浪费在等待上。

## Q4：promise/future 为什么不是最终方案？

它能表达异步结果，但 `future.get()` 在结果未就绪时会阻塞 OS 线程。若该线程还负责收包会直接死锁；增加后台接收线程又会带来线程、锁和调度成本。

## Q5：协程为什么能避免线程阻塞？

Boost.Coroutine2 在用户态保存当前协程的栈和寄存器。RPC 等待时执行 `yield()`，只暂停当前协程并把控制权交还 EventLoop；OS 线程仍可处理其他网络事件。响应到达后再恢复原协程。

## Q6：为什么不需要 mutex？

请求登记、网络读写、响应匹配、协程恢复和连接关闭都由同一个 EventLoop 线程执行。pending map 是线程独占数据，无锁安全来自线程归属，而不是容器本身。

## Q7：为什么响应可能乱序？

协议不能假设服务端永远按请求顺序完成。不同请求可能具有不同处理耗时，未来也可能由异步服务或不同执行单元完成。因此响应帧必须携带原请求的 `sequenceId`。

## Q8：连接断开时如何避免协程永久挂起？

遍历全部 PendingCall，写入连接错误并把每个协程投递回 EventLoop。协程恢复后抛出异常，然后清空 pending map。

# 一分钟面试表达

> 最初如果一个并发请求使用一个线程和一条连接，会产生较高的线程调度、fd 和建连开销；如果只用一条连接做同步 Ping-Pong，又会在每次 RTT 中串行等待。我的做法是在二进制帧头中加入长度字段和 sequenceId：长度字段负责 TCP 半包、粘包边界，sequenceId 负责请求与响应关联。客户端允许多个请求连续进入同一条连接，并维护 sequenceId 到响应对象和等待协程的 pending map。调用发出后通过 Boost.Coroutine2 yield，只挂起当前协程，不阻塞 EventLoop 线程；epoll 收到响应后解出完整帧，按 sequenceId 找到对应上下文，再在原 Loop 中恢复协程。所有状态由一个 EventLoop 线程独占，因此不需要 mutex。这样以一条长连接和较少线程承载多个并发 RPC 等待，同时业务代码仍然保持同步调用形式。
