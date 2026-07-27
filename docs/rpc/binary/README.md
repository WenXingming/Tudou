# Binary RPC

对应源码：`src/tudou/rpc/binary`。

## 模块职责

Binary RPC 使用 Protobuf 表达元数据和消息，通过显式帧边界承载请求、响应和异常信息。它负责编码、解码、服务路由、单连接多路复用以及 TcpConnection 适配。

## 核心对象

- `BinaryRpcCodec`：Protobuf 元数据和 payload 的编码/解码。
- `BinaryRpcFrame`：20 字节二进制帧头、消息类型与长度字段定义。
- `BinaryRpc.proto`：定义 `RpcMeta`（服务名 `service_name` 与方法名 `method_name`）及示例服务规范。
- `BinaryRpcChannel`：客户端非阻塞连接、请求序列号 `sequenceId` 和并发响应关联。
- `BinaryRpcRouter`：按 service/method 注册和分发 Protobuf 服务。
- `BinaryRpcServer`：把 TcpServer 消息回调接到协议分发。

## 帧结构与 Payload 说明

```text
┌─────────────────────────┬──────────────────────┬──────────────────────┐
│  20 字节固定帧头 Header  │   RpcMeta (元数据)    │  Business Body (载荷) │
│ (magic, seq, sizes...)  │ (service_name/method)│ (具体 Protobuf 消息) │
└─────────────────────────┴──────────────────────┴──────────────────────┘
```

- **RpcMeta (`BinaryRpc.proto`)**：存放目标服务全名和方法名，供服务端 `BinaryRpcRouter` 路由分发。
- **Business Body**：
  - **请求包**：存放 RPC 函数的输入参数（即业务 Protobuf Request 消息序列化流）。
  - **响应包**：存放 RPC 函数的返回值（即业务 Protobuf Response 消息序列化流）。

## 为什么需要 RpcMeta（元数据设计考量）

1. **解决自描述缺失问题**：Protobuf 序列化后的 Body 只有字段 Tag 和 Value，字节流本身不携带“发给哪个 Service/Method”的信息。必须通过 `RpcMeta` 补全自描述信息，服务端才能反序列化为正确的 Request 对象并完成路由回调。
2. **零成本扩展性**：采用 Protobuf `message RpcMeta` 表达元数据而非把字段硬编码在 C++ 二进制帧头中，使得后续追加 `trace_id`（链路追踪）、`timeout_ms`（超时控制）、`compress_type`（压缩类型）等元数据时，无需破坏低层帧头格式与二进制兼容性。

## 面试主线

```text
TcpConnection 字节流
  → BinaryRpcCodec 解析帧 (Header + RpcMeta + Body)
  → sequence/request id 关联响应
  → BinaryRpcRouter 根据 RpcMeta (service_name/method_name) 找到对应函数
  → Protobuf Service::CallMethod
  → 编码响应并发送
```

深入内容从 [RPC 主文档](<../README.md>) 和 [单连接多路复用](<../RPC 单连接多路复用.md>) 开始。
