# Binary RPC

对应源码：`src/tudou/rpc/binary`。

## 模块职责

Binary RPC 使用 Protobuf 表达元数据和消息，通过显式帧边界承载请求、响应和异常信息。它负责编码、解码、服务路由、单连接多路复用以及 TcpConnection 适配。

## 核心对象

- `BinaryRpcCodec`：Protobuf 元数据和 payload 的编码/解码。
- `Protocol`：帧头、长度和协议字段定义。
- `BinaryRpcChannel`：客户端连接、请求序列号和并发响应关联。
- `BinaryRpcRouter`：按 service/method 注册和分发 Protobuf 服务。
- `BinaryRpcServer`：把 TcpServer 消息回调接到协议分发。

## 面试主线

```text
TcpConnection 字节流
  → BinaryRpcCodec 解析帧
  → sequence/request id 关联响应
  → BinaryRpcRouter 找到 service/method
  → Protobuf Service::CallMethod
  → 编码响应并发送
```

深入内容从 [RPC 主文档](<../README.md>) 和 [单连接多路复用](<../RPC 单连接多路复用.md>) 开始。
