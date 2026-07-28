# BinaryRpc - Router 设计：Protobuf 反射分发、CallMethod 与同步完成契约

`binary::Router` 接收不含网络状态的逻辑 `Request`，根据 service 和 method 找到用户注册的 Protobuf Service，**动态创建正确类型的请求与响应对象**，执行业务方法并返回序列化后的响应 body。

# Situation — 情境

服务端从完整 RPC 帧中只能得到三个逻辑字段：

```text
serviceName
methodName
序列化后的 Protobuf body
```

Router 在编译框架时并不知道业务会定义哪些 Service，也不知道每个方法对应哪种 Request 和 Response。若 Server 使用大量 `if/switch` 硬编码业务方法，每新增一个 RPC 接口都必须修改网络框架。

**Protobuf 的 Service、Descriptor、Prototype 和 `CallMethod()` 提供了运行时反射分发能力**，但还需要明确两个容易混淆的问题：

- `service->CallMethod()` 是谁定义的，它怎样调用用户业务函数；
- `SynchronousCompletion` 为什么存在，`done->Run()` 又代表什么。

# Task — 任务

Router 需要用一个直接接口完成一次逻辑调用：

```cpp
void register_service(std::shared_ptr<google::protobuf::Service> service);
std::string dispatch(const Request& request) const;
```

同时保持以下边界：

- 不知道 Socket、TcpConnection、Frame 和 sequenceId；
- 不硬编码任何具体业务 Service 或 Message 类型；
- Service 在服务器启动前完成注册；
- 当前只支持在 `CallMethod()` 返回前完成的同步业务方法；
- 路由、反序列化或业务异常统一交给 Server 关闭连接。

# Action — 设计

## 1. Request 是网络层与业务路由的分界

Server 从 `Frame.head` 解析 `CallHead`，再构造：

```cpp
Request(
    head.service_name(),
    head.method_name(),
    frame.body);
```

Request 不携带 `sequenceId`、连接指针或帧类型。Router 因此只处理：

```text
Request { serviceName, methodName, body }
    -> response body
```

响应属于哪条连接、使用哪个 sequenceId 回包，仍由 Server 负责。

## 2. 业务 Service 由使用者定义并注册

业务接口首先写在 `.proto` 中：

```proto
service TestEchoService {
    rpc Echo(EchoRequest) returns (EchoResponse);
}
```

启用 `option cc_generic_services = true` 后，`protoc` 会生成继承自 `google::protobuf::Service` 的基类，其中包含具体业务方法和通用 `CallMethod()`：

```cpp
class TestEchoService : public google::protobuf::Service {
public:
    virtual void Echo(
        google::protobuf::RpcController* controller,
        const EchoRequest* request,
        EchoResponse* response,
        google::protobuf::Closure* done);

    void CallMethod(
        const google::protobuf::MethodDescriptor* method,
        google::protobuf::RpcController* controller,
        const google::protobuf::Message* request,
        google::protobuf::Message* response,
        google::protobuf::Closure* done) override;
};
```

应用程序继承该基类并实现真正业务逻辑：

```cpp
class EchoService : public TestEchoService {
public:
    void Echo(google::protobuf::RpcController*,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) override {
        // 这里才是真正的业务逻辑。
        response->set_message("Echo: " + request->message());
        done->Run();
    }
};
```

然后在 Server 启动前注册：

```cpp
binary::Server server("127.0.0.1", 8080, 4);
server.register_service(std::make_shared<EchoService>());
server.start();
```

当前仓库中的具体二进制 RPC 业务实现主要位于 `BinaryRpcServerTest`、`BinaryRpcChannelTest` 和 `UnifiedRpcServerTest`。框架源码不内置业务逻辑，实际应用应自行实现并注册 Service。

## 3. 注册表使用完整 Service 名称

Router 保存：

```cpp
std::unordered_map<
    std::string,
    std::shared_ptr<google::protobuf::Service>
> services_;
```

注册键来自 Descriptor：

```cpp
const auto* descriptor = service->GetDescriptor();
services_[descriptor->full_name()] = std::move(service);
```

例如：

```text
tudou.rpc.test.TestEchoService -> shared_ptr<EchoService>
```

Router 持有基类指针，但实际对象仍是用户实现的 `EchoService`。使用 `shared_ptr` 是因为 Router 必须保证 Service 存活，而且 UnifiedRpcServer 会把同一个 Service 同时注册到二进制 Router 和 JSON 桥接 Router。

## 4. dispatch() 的完整反射流程

`dispatch()` 保持线性：

```text
按 serviceName 查找 Service
    ↓
从 ServiceDescriptor 查找 MethodDescriptor
    ↓
根据 request prototype 创建具体 Request Message
    ↓
反序列化 request.body
    ↓
根据 response prototype 创建具体 Response Message
    ↓
调用 Service::CallMethod()
    ↓
确认业务同步完成
    ↓
序列化 Response Message
```

动态创建 Message 的关键代码是：

```cpp
std::unique_ptr<google::protobuf::Message> protobufRequest(
    service->GetRequestPrototype(method).New());

std::unique_ptr<google::protobuf::Message> protobufResponse(
    service->GetResponsePrototype(method).New());
```

如果 `method` 是 `Echo`，两个 Prototype 分别生成 `EchoRequest` 和 `EchoResponse`。具体类型只能在运行时根据 MethodDescriptor 确定，因此这里使用 `unique_ptr<Message>` 是必要的动态类型和 RAII 管理，不是多余堆分配。

## 5. CallMethod 是 Protobuf 生成的通用分发入口

`service->CallMethod()` 不是用户直接编写的业务函数。它由 `protoc` 生成，内部根据 MethodDescriptor 的索引选择具体虚函数：

```cpp
void TestEchoService::CallMethod(
    const MethodDescriptor* method,
    RpcController* controller,
    const Message* request,
    Message* response,
    Closure* done) {

    switch (method->index()) {
    case 0:
        Echo(
            controller,
            static_cast<const EchoRequest*>(request),
            static_cast<EchoResponse*>(response),
            done);
        break;
    }
}
```

`Echo()` 是虚函数，因此最终会动态调用用户子类中的 override：

```text
Router::dispatch()
    -> protoc 生成的 TestEchoService::CallMethod()
    -> 根据 MethodDescriptor 选择 Echo()
    -> 用户实现的 EchoService::Echo()
    -> 真正业务逻辑
```

Router 只需要认识统一的 `google::protobuf::Service`，就能调用任意 `.proto` 生成的 Service。

## 6. SynchronousCompletion 是完成标记

Protobuf Service 同时允许同步和异步实现，所以具体业务方法没有返回响应，而是接收一个 `done` 回调。业务填完 response 后调用：

```cpp
done->Run();
```

当前 Router 使用一个极小的完成标记：

```cpp
class SynchronousCompletion : public google::protobuf::Closure {
public:
    SynchronousCompletion() : completed_(false) {
    }

    void Run() override {
        completed_ = true;
    }

    bool completed() const {
        return completed_;
    }

private:
    bool completed_;
};
```

它不执行业务、不发送响应，也不拥有 Message，只记录 `done->Run()` 是否已经发生。

完整时间线如下：

```text
Router 创建 completion(false)
    ↓
CallMethod 转发到用户业务方法
    ↓
业务读取 request，填写 response
    ↓
业务调用 done->Run()
    ↓
completion 变成 true
    ↓
业务方法和 CallMethod 返回
    ↓
Router 检查 completed() 并序列化 response
```

如果 `CallMethod()` 返回时仍未完成：

```cpp
if (!completion.completed()) {
    throw std::runtime_error(
        "Router: Asynchronous services are not supported");
}
```

这个检查用于维护同步 Service 契约。它不是异步生命周期管理器，也不能让异步 Service 安全运行。

## 7. 为什么当前只支持同步 Service

`dispatch()` 中的 request、response 和 completion 都由当前函数作用域管理。真正的异步业务可能在 `CallMethod()` 返回后继续使用这些对象，因此必须额外设计：

- 堆上的请求上下文；
- response 和 completion 的共享生命周期；
- 跨线程回到正确 EventLoop；
- 连接关闭后的取消和失效处理；
- 异步完成后编码并发送响应。

当前项目没有这项需求，因此明确要求业务在 `CallMethod()` 返回前填写 response 并调用 `done->Run()`。这使 Router 可以直接返回响应字符串，避免引入异步状态机。

## 8. 并发和执行线程边界

Service 应在 Server 启动前注册；启动后 Router 只并发读取 `services_`，不再修改映射。因此 Router 不需要 mutex。

业务方法直接运行在收到请求的 EventLoop 线程中。同一个 Service 对象可能被多个 I/O 线程并发调用，所以用户实现的 Service 必须保证自身线程安全；耗时业务也会阻塞该 EventLoop 管理的其他连接。

当前 `RpcController` 传入 `nullptr`，表示尚未实现超时、取消和结构化错误状态。未知 Service、未知 Method、非法请求体或业务异常由 Server 统一记录并关闭连接，不发送专门的 RPC 错误帧。

# Result — 结果

- Server 不接触 Protobuf Descriptor、Prototype 和动态 Message；
- Router 不知道 Socket、Frame、sequenceId 和响应连接；
- 新增业务方法只需修改 `.proto`、实现生成的 Service 并注册，无需修改 Router；
- request 和 response 使用 `unique_ptr` 自动管理动态类型；
- 同步完成契约避免了异步请求上下文和跨线程响应状态机。

单元测试覆盖正常分发、未知 Service、未知 Method、非法请求体以及未同步完成的 Service。

# 权衡

- 重复注册同名 Service 会覆盖旧对象；当前只允许启动前注册，因此保留这个直接策略。
- Router 使用异常报告失败，Server 采用协议错误即关闭连接的 fail-closed 策略。
- 当前不支持异步 Service、RpcController 和 RPC 错误响应；这些是明确边界，不是假装已经实现的能力。
- `dispatch()` 保持一条线性反射流程，不继续提取一串只转发一次的 helper。

# 面试核心问答

### 1. RPC 的业务逻辑写在哪里？

业务逻辑写在应用程序自定义的 Service 实现中。使用者继承 `protoc` 生成的 Service 基类，重写 `Echo`、`Login` 等具体方法，然后把实例注册进 `binary::Server`。Router 和 Server 都不内置业务逻辑。

### 2. `service->CallMethod()` 是具体业务函数吗？

不是。它是 `protoc` 生成的统一分发入口，根据 MethodDescriptor 找到具体虚函数，再调用用户 override 的业务方法。真正的业务函数是 `EchoService::Echo()` 这类实现。

### 3. CallMethod 在哪里定义？

声明和定义位于 `protoc` 生成的 `.pb.h/.pb.cc`。启用 `cc_generic_services` 后，每个 Protobuf Service 都会生成自己的 `CallMethod()`、`GetDescriptor()`、`GetRequestPrototype()` 和 `GetResponsePrototype()`。

### 4. Router 为什么不用 switch 判断 service 和 method？

ServiceDescriptor 和 MethodDescriptor 已经保存了运行时元数据，生成的 CallMethod 也负责按方法索引转发。Router 使用反射后可以处理任意业务 Service，无需随业务接口一起修改。

### 5. `SynchronousCompletion` 有什么作用？

它是一个布尔完成标记。Router 把它作为 `done` 传给业务方法，业务调用 `done->Run()` 后标记变为 true；Router 只有确认它在 `CallMethod()` 返回前变为 true，才序列化 response。

### 6. 为什么业务方法填写完 response 后还必须调用 `done->Run()`？

因为 Protobuf Service API 同时支持同步和异步实现，response 填写与“调用已经完成”是两个独立信号。当前 Router 依靠 done 明确确认同步业务已经结束。

### 7. 如果业务方法不调用 `done->Run()` 会怎样？

Router 会认为该调用没有同步完成并抛出异常，Server 随后关闭连接。若业务把 request、response 或 done 保存到其他线程稍后使用，则违反当前同步契约，可能访问已经析构的对象。

### 8. 为什么 Router 不直接支持异步 Service？

异步完成需要把请求、响应、连接和完成回调一起延长到堆上，并处理跨线程发送和连接关闭。当前项目选择同步 Service，使调用链和生命周期保持直接；未来有真实需求时应单独设计异步响应上下文。

### 9. Router 为什么保存 `shared_ptr<Service>`？

Router 必须保证注册的 Service 在请求期间存活，而且 UnifiedRpcServer 会让二进制和 JSON 桥接 Router 共享同一个 Service。这里确实存在共享所有权。

### 10. Router 可以并发 dispatch 吗？

可以，但前提是启动后不再调用 `register_service()`，并且用户实现的 Service 本身线程安全。Router 的映射只有并发读取，同一个业务 Service 则可能被多个 EventLoop 同时调用。

# 面试表达

> Router 接收不含网络状态的 Request，使用 Protobuf Descriptor 找到 Service 和 Method，再用 Prototype 创建具体请求与响应 Message。`service->CallMethod()` 是 protoc 生成的通用分发入口，它最终调用用户重写的业务方法。业务填写 response 后调用 `done->Run()`，SynchronousCompletion 只记录这次调用是否在返回前完成。当前明确只支持同步 Service，从而让 Message 由 RAII 管理，避免异步请求上下文和跨线程响应状态机。
