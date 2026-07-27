# RPC 模块

## 模块职责

RPC 层在 TCP/Buffer 之上提供 JSON-RPC 和基于 Protobuf 的二进制 RPC，负责协议分帧、请求响应关联、连接复用、协程调用和服务端组合。

## 源码结构

```text
src/tudou/rpc/
  Coroutine.*
  UnifiedRpcServer.*
  binary/
  json/
```

协议子模块入口：

- [Binary RPC](<binary/README.md>)
- [JSON RPC](<json/README.md>)

## 核心流程

```text
TcpConnection::receive
  → RPC 分帧
  → 根据 request id / stream id 匹配请求
  → 编解码 JSON 或 Protobuf
  → 调用服务处理器
  → TcpConnection::send
```

## 关键设计主题

- TCP 是字节流，RPC 必须在应用层定义长度或帧边界。
- 单连接多路复用通过请求标识把并发请求和响应重新关联。
- JSON RPC 便于调试，Protobuf 二进制 RPC 更适合稳定协议和高吞吐场景。
- 协程封装客户端等待过程，降低异步回调嵌套，但底层网络线程归属不变。
- `UnifiedRpcServer` 统一组合 JSON 和二进制服务入口。

当前代码已经提供协议、服务端路由、单连接多路复用和协程相关能力；服务发现、多实例负载均衡和连接池仍属于设计储备，不应在面试中说成已经实现。

## 深入文档

- [RPC 拆包粘包与分帧](<RPC 拆包粘包处理.md>)
- [单连接多路复用](<RPC 单连接多路复用.md>)
- [RPC 协程化思考](<RPC协程化思考与设计.md>)
- [有栈协程调用全路径](<Tudou RPC 有栈协程实现与调用全路径.md>)
- [服务发现与多实例负载均衡](<RPC 服务发现与多实例负载均衡设计.md>)
- [协议图](<assets/tudou_rpc_protocol.jpg>)
