# UnifiedRpcServer 设计：双轨服务端桥接与 Protobuf 反射互转

`UnifiedRpcServer` 将同一个同步 Protobuf Service 同时暴露为二进制 RPC 与 JSON-RPC，负责协议组合，不重新实现业务调用。

# Situation — 情境

内部服务适合使用紧凑的 Protobuf 二进制协议，调试和跨语言接入又常需要可读的 JSON-RPC。若两套入口分别注册业务函数，服务名、方法名和参数转换会重复维护。

# Task — 任务

统一服务端需要一次注册 Service，同时启动两个网络端点；JSON 请求应转换为对应的 Protobuf Message，并复用与二进制入口相同的 Router 调用语义。

# Action — 设计

## 值成员表达必然存在

```cpp
binary::Server binaryServer_;
JsonRpcServer jsonServer_;
binary::Router jsonBridgeRouter_;
std::thread binaryThread_;
```

两个 Server 和桥接 Router 都是对象生命周期内必然存在的协作者，因此使用值成员，不使用 nullable `unique_ptr` 和额外初始化状态。

## 一次注册，两个入口

`register_service()` 先把 Service 注册到二进制 Server 和桥接 Router，再遍历 Descriptor 中的方法，为 JSON Server 注册 `完整服务名.方法名`。

JSON 方法被调用时：

```text
JSON params
  -> JsonStringToMessage(request prototype)
  -> binary::Router::dispatch(Request)
  -> response protobuf bytes
  -> MessageToJsonString(response prototype)
  -> JSON result
```

桥接层复用 `binary::Router`，不再复制一遍 Service 查找、动态 Message 创建、done closure 和序列化逻辑。

## 线程安排

二进制 Server 在成员线程中运行，JSON Server 在调用 `start()` 的线程运行。`stop()` 依次停止两个 Server，并 join 二进制线程。Service 在启动前注册，运行期只读。

# Result — 结果

- 一个 Protobuf Service 同时提供紧凑二进制入口和可读 JSON 入口；
- JSON/Protobuf 转换只位于协议桥接边界；
- 业务反射调用复用 Router，没有两套分发实现；
- 值成员删除了延迟初始化、空指针检查和重复配置状态。

# 面试表达

> UnifiedRpcServer 是组合根：持有二进制 Server、JSON Server 和一个用于 JSON 桥接的 Router。注册一次 Protobuf Service 后，通过 Descriptor 自动暴露所有 JSON 方法；JSON 只负责与 Protobuf Message 互转，真正的方法查找和调用仍复用 Router。
