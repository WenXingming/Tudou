# 整体架构与跨模块原则

Tudou 是一个基于 Linux `epoll` 的 C++ Reactor 网络框架。核心做法是：每个 `EventLoop` 在线程内串行处理事件，跨线程操作通过任务队列投递，TCP、HTTP 和 RPC 在这个事件模型上逐层构建。

## 模块关系

```text
应用业务
  ↓
HTTP / RPC
  ↓
TCP
  ↓
Timer + Reactor
  ↓
Linux socket / epoll / timerfd
```

## 跨模块设计原则

- **线程归属**：`Channel`、`TcpConnection`、`TimerQueue` 的可变状态归所属 `EventLoop`。
- **消息传递**：其他线程通过 `run_in_loop()` / `queue_in_loop()` 请求 owner loop 执行操作。
- **RAII 所有权**：fd 使用 `Socket` / `ScopedFd` 管理，拥有关系用 `unique_ptr` 或 `shared_ptr` 表达。
- **职责分层**：Reactor 只分发事件，TCP 只处理字节流，HTTP/RPC 负责协议和业务映射。
- **代码简洁**：优先减少状态、转发层和隐式生命周期，而不是提前抽象接口。

## 跨模块文档

- [项目架构图](<Architecture.md>)
- [回调与控制反转](<深入理解回调.md>)
- [生命周期、RAII 与回调安全](<生命周期管理详解.md>)
- [依赖关系图](<assets/dependency_graph.svg>)
