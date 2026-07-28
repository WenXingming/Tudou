# BinaryRpcChannel 设计：单连接多路复用与并发请求关联

二进制 RPC 提供两个具体客户端：`binary::Channel` 是阻塞式客户端，`binary::CoroutineChannel` 是 EventLoop 驱动的非阻塞协程客户端。二者共享 `FrameCodec`、`Connection` 和 `Multiplexer`，不共享模式状态机。

# Situation — 情境

多个调用线程或协程可能复用一条 TCP 长连接。响应到达顺序不一定与请求发送顺序相同；连接关闭时所有等待者也必须立即失败。阻塞 I/O 与 Reactor I/O 的生命周期、线程模型不同，塞进一个 Channel 会产生大量模式分支和可空成员。

# Task — 任务

客户端需要解决：请求完整写入不交叉、响应按 sequenceId 匹配、半包粘包处理、连接失败唤醒全部 pending call；同时让阻塞和协程实现各自保持单一线程模型。

# Action — 设计

## Multiplexer：只做请求关联

```cpp
uint64_t register_call(Completion completion);
bool complete_call(uint64_t sequenceId, const std::string& body);
void close(std::exception_ptr error);
```

它维护 `sequenceId -> Completion`。响应到达时先在锁内移动并删除 completion，再在锁外执行；连接关闭时一次取出全部 completion 并传播异常。它不知道 Socket、Protobuf、Future 或 Coroutine。

## 阻塞 Channel

调用线程注册 promise、串行写入完整请求，然后 `future.get()`；唯一后台接收线程持续读取响应并交给 Connection 拆帧，再由 Multiplexer 完成正确的 promise。

发送锁只保证多个线程写入的帧字节不交叉，不包围等待和解析。析构时先 `shutdown` 唤醒阻塞 read，再关闭 Multiplexer，最后 join 接收线程。

## CoroutineChannel

协程版套接字为 nonblocking，由 Reactor Channel 监听读写事件。调用流程是：

```text
register completion -> queue encoded request -> yield
EPOLLOUT -> flush output Buffer
EPOLLIN  -> decode response -> complete call -> queue resume
```

所有网络状态只在所属 EventLoop 线程访问，因此不需要发送锁或后台接收线程。调用必须发生在绑定同一 EventLoop 的 `binary::Coroutine` 中。

## 为什么拆成两个类

`Channel` 只有阻塞 fd、后台线程、future 和发送锁；`CoroutineChannel` 只有 nonblocking fd、Reactor Channel、输出 Buffer 和协程恢复。没有 `Mode`、没有“只有某模式才非空”的成员，也没有一个函数内的双分支主流程。

# Result — 结果

- 单 TCP 连接支持并发请求和乱序响应；
- 连接关闭会唤醒全部等待者，不留下永久阻塞；
- 阻塞与协程网络模型各自独立，核心组件仍可复用；
- Multiplexer 可脱离网络单测乱序完成和批量失败。

# 面试表达

> sequenceId 负责协议层关联，Multiplexer 负责并发 pending call 管理。阻塞客户端用后台接收线程加 future，协程客户端用 epoll 回调加 yield/resume；我没有用 Mode 状态机强行统一两种生命周期，而是让两个具体 Channel 复用同一套 FrameCodec、Connection 和 Multiplexer。
