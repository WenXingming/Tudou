# RPC 模块设计文档

RPC 层建立在 TCP、Buffer 和 Reactor 之上，提供 Protobuf 二进制 RPC、JSON-RPC，以及同时暴露两种协议的统一服务端。

## 源码结构

```text
src/tudou/rpc/
  binary/
    Frame.h                    # 与上层交换的完整逻辑帧
    FrameCodec.h/cpp           # 20 字节线协议与 Frame 的转换
    Connection.h/cpp           # 单连接半包缓存与粘包拆分
    Request.h                  # Router 使用的逻辑请求
    Router.h/cpp               # Protobuf Service 反射分发
    Server.h/cpp               # TCP、分帧、路由与回包的编排
    Multiplexer.h/cpp          # sequenceId 与并发调用的关联
    Channel.h/cpp              # 阻塞客户端与后台接收线程
    Coroutine.h/cpp            # Boost.Coroutine2 执行上下文
    CoroutineChannel.h/cpp     # EventLoop 驱动的非阻塞协程客户端
    binary_rpc.proto           # service/method 调用头
  json/
    Client.h/cpp
    Router.h/cpp
    Server.h/cpp
  UnifiedRpcServer.h/cpp       # 二进制 RPC 与 JSON-RPC 双端口组合
```

类型放在 `tudou::rpc::binary` 命名空间，因此服务端、路由器等使用 `binary::Server`、`binary::Router` 这样的短名称；帧编解码器使用职责更明确的 `binary::FrameCodec`。

## 二进制 RPC 的职责链

```text
客户端业务
  -> Channel / CoroutineChannel：发起调用
  -> FrameCodec：编码 Frame
  -> TCP 字节流
  -> Connection：累计半包、拆出全部完整 Frame
  -> Server：解析 CallHead 并编排请求
  -> Router：查找 Protobuf Service/Method 并同步执行
  -> FrameCodec：编码响应
  -> Multiplexer：按 sequenceId 唤醒正确的客户端调用
```

关键边界：

- `FrameCodec` 只知道帧与字节，不保存连接状态；
- `Connection` 只处理 TCP 半包、粘包，不知道网络、路由或回调；
- `Router` 只处理逻辑请求，不携带帧序列号；
- `Server` 只展示编排流程，不重复实现分帧和 Protobuf 反射；
- 阻塞客户端与协程客户端是两个具体类，不用模式状态机塞进同一个 Channel；
- 当前服务端只支持同步 Protobuf Service，即 `CallMethod()` 返回前必须调用 `done->Run()`。

## 重点文档

1. [BinaryRpc - Frame 设计：20 字节固定帧头、内存对齐与大端字节序](<BinaryRpc - Frame 设计：20 字节固定帧头、内存对齐与大端字节序.md>)
2. [BinaryRpc - FrameCodec 设计：帧编解码、Buffer 预窥探与半包不消费](<BinaryRpc - FrameCodec 设计：帧编解码、Buffer 预窥探与半包不消费.md>)
3. [BinaryRpc - Connection 设计：连接级 Buffer、半包保留与粘包拆分](<BinaryRpc - Connection 设计：连接级 Buffer、半包保留与粘包拆分.md>)
4. [BinaryRpc - Router 设计：Protobuf 反射分发、CallMethod 与同步完成契约](<BinaryRpc - Router 设计：Protobuf 反射分发、CallMethod 与同步完成契约.md>)
5. [BinaryRpc - Server 设计：Reactor 编排、连接状态与错误关闭](<BinaryRpc - Server 设计：Reactor 编排、连接状态与错误关闭.md>)
6. [BinaryRpcChannel 设计：单连接多路复用与并发请求关联](<BinaryRpcChannel 设计：单连接多路复用与并发请求关联.md>)
7. [BinaryRpcCoroutine 设计：Boost.Coroutine2 有栈协程挂起与恢复](<BinaryRpcCoroutine 设计：Boost.Coroutine2 有栈协程挂起与恢复.md>)
8. [UnifiedRpcServer 设计：双轨服务端桥接与 Protobuf 反射互转](<UnifiedRpcServer 设计：双轨服务端桥接与 Protobuf 反射互转.md>)

补充资料：[Protobuf 设计与使用指南](<Protobuf 设计与使用指南.md>)、[JSON-RPC 设计与实现](<JSON-RPC 设计与实现：分帧、粘包拆包与协议路由.md>)。
