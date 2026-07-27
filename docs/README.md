# 面试讲解入口

面试资料以各模块 `README.md` 为当前实现的唯一来源，不再直接依赖旧的综合问答文件。

## 推荐讲解顺序

```text
项目定位
  → Reactor：EventLoop + epoll + Channel
  → Timer：timerfd + EventLoop
  → TCP：owner loop + 非阻塞收发 + shutdown
  → HTTP/TLS：流式解析 + Router + TLS
  → RPC：分帧 + 多路复用 + 协程
```

## 两分钟项目介绍

Tudou 是一个基于 Linux epoll 和 C++ 的高性能网络框架。底层用 EventLoop 驱动 Poller、Channel、跨线程任务和 timerfd；TCP 层把每条连接固定到 owner EventLoop，负责非阻塞收发、writev、心跳和生命周期；HTTP 层提供 llhttp 流式解析、Router 和 TLS；RPC 层提供 JSON/Protobuf 分帧、多路复用和协程调用。

项目最重要的设计不是“到处加锁”，而是通过 one loop per thread 固定可变状态的 owner。跨线程操作通过任务队列投递，连接表按 EventLoop 分片，心跳跟随 TcpConnection。这样可以同时获得清晰的生命周期边界和较低的锁竞争。

## 面试准备方式

每个模块至少准备四个答案：

1. 它解决什么问题；
2. 一条完整调用流程；
3. 一个最值得讲的设计取舍；
4. 一个明确的限制或未实现能力。

旧版综合问答和历史重构记录位于 `docs/archive/`，只用于了解演进，不作为当前实现答案。
