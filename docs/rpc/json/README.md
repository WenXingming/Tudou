# JSON RPC

对应源码：`src/tudou/rpc/json`。

## 模块职责

JSON RPC 面向可读性和调试便利性，负责 JSON-RPC 请求解析、方法注册、参数分发和响应封装。底层连接和线程模型仍由 TCP/Reactor 提供。

## 核心对象

- `JsonRpcClient`：发起请求并等待对应响应。
- `JsonRpcRouter`：按方法名注册处理器并完成参数分发。
- `JsonRpcServer`：把 TcpServer 消息回调接入 JSON-RPC 解析。

## 面试主线

```text
TcpConnection 字节流
  → JSON-RPC 消息边界
  → method 查找
  → params 交给处理器
  → result/error 封装
  → TcpConnection::send
```

JSON RPC 与 Binary RPC 共享 TCP/线程边界，但在编码格式和服务注册接口上独立。通用协议取舍见 [RPC 主文档](<../README.md>)。
