# Protobuf 设计与使用指南

Protocol Buffers（简称 Protobuf）是 Google 开发的高效、跨语言、跨平台的**结构化数据序列化协议**。在 Tudou RPC 模块中，Protobuf 被作为二进制传输的数据载荷格式与 Service 服务定义语言（IDL）。

# Situation — 情境

在分布式系统与 RPC 框架中，经常需要在网络中传输结构化数据（如用户对象、请求参数、返回结果）。常见的序列化方案对比：

1. XML：文本格式，包含大量冗余标签，体积庞大，解析极慢；
2. **JSON**：文本格式，人类可读性好，但每个数据包中都必须重复携带 key 名字（如 `"username": "alice"`），占用**大量网卡带宽**，且反序列化需要**复杂的字符串解析**；
3. C++ 原生 struct 拷贝：直接发送 `memcpy` 结构体，性能极高，但无法跨语言（Python 无法直接读 C++ 结构体），且由于内存补齐（Padding）和编译器差异，无法向前/向后兼容。

Protobuf 结合了**二进制的高性能**与**强类型的跨语言兼容性**。

# Task — 任务

Protobuf 在 Tudou 框架中承担两个核心任务：

1. **数据序列化与反序列化**：将 C++ 业务对象编码为紧凑的二进制字节流（`SerializeToString`），并在接收端安全还原（`ParseFromString`）；
2. **RPC 接口定义语言 (IDL)**：使用 `.proto` 文件统一声明 Service 接口与数据 Message，并自动生成 C++ 虚函数基类与客户端 Stub。

Protobuf 不是完整的 RPC 框架。它不负责 TCP 连接、消息分帧、半包粘包、请求匹配、超时重试或线程调度。在 Tudou 中，这些能力分别由 `FrameCodec`、`Connection`、`Router` 和 `CoroutineChannel` 提供：

```text
Protobuf → 定义消息与 Service，生成类型，完成消息编解码
Tudou    → 组织 RPC Frame，网络收发，路由调用，协程挂起与恢复
```

# Action — 设计与使用工作流

## 工作流全景图

```text
  ┌─────────────────────────┐
  │   1. 编写 echo.proto    │  (定义消息 Message 与 Service 接口)
  └────────────┬────────────┘
               │ protoc 编译器编译
               ▼
  ┌─────────────────────────┐
  │ 2. 生成 echo.pb.h / cc  │  (自动生成 C++ 类与 Service 虚基类)
  └────────────┬────────────┘
               │ 业务代码继承与调用
               ▼
  ┌─────────────────────────┐
  │ 3. 业务代码与 RPC 管道  │  (调用 SerializeToString / CallMethod)
  └─────────────────────────┘
```

## 步骤一：编写 `.proto` 文件

以 [`examples/BinaryRpc/echo.proto`](../../examples/BinaryRpc/echo.proto) 为例：

```protobuf
syntax = "proto3";

package tudou.example;

// 开启 C++ 通用 RPC 服务代码生成
option cc_generic_services = true;

// 定义请求消息体
message EchoRequest {
    string message = 1; // 1 为字段唯一 Tag 编号（非常关键！）
}

// 定义响应消息体
message EchoResponse {
    string message = 1;
}

// 定义 RPC 服务接口
service EchoService {
    rpc Echo(EchoRequest) returns (EchoResponse);
}
```

### 关键语法点：

- **Tag 编号 (`= 1`, `= 2`)**：二进制传输时不会发送 `"message"` 这样的字段名，而是编码字段编号和 wire type，这是 Protobuf 比 JSON 更紧凑的重要原因。
- **`option cc_generic_services = true;`**：指示 `protoc` 生成继承自 `google::protobuf::Service` 的 C++ RPC 虚基类与 Stub。
- **`EchoService`**：服务名，表示一组相关 RPC 方法。
- **`Echo`**：`rpc` 后、括号前的内容就是方法名，不能省略。
- **`EchoRequest` / `EchoResponse`**：分别是该方法的请求和响应消息类型。

### Tag 在线上怎样表示

Tag 不会以字段名字符串发送。线上字段头由“字段编号 + wire type”共同编码。例如：

```protobuf
message EchoRequest {
    string message = 1;
}
```

当 `message = "A"` 时，消息字节是：

```text
0A 01 41
│  │  └─ 字符 A
│  └──── 字符串长度 1
└─────── 字段编号 1 + length-delimited wire type
```

接收方根据字段编号知道这是 `message`，根据 wire type 知道后面是长度加内容。未知字段也能按照 wire type 跳过，这正是版本兼容的基础。

### 常用字段会生成什么接口

```protobuf
message User {
    int64 id = 1;
    repeated string roles = 2;
    optional string email = 3;
}
```

`protoc` 会生成强类型 C++ 接口：

```cpp
User user;
user.set_id(1);
user.add_roles("admin");
user.set_email("alice@example.com");

user.id();
user.roles_size();
user.roles(0);
user.has_email();
```

普通字段提供 getter/setter，`repeated` 表示可重复字段，`optional` 可以区分“没有设置”和“设置为默认值”。

完整方法名由 package、service 和 method 组成：

```text
tudou.example.EchoService.Echo
└── package ──┘ └ service ┘ └ method
```

一个 Service 可以包含多个有名字的方法：

```protobuf
service EchoService {
    rpc Echo(EchoRequest) returns (EchoResponse);
    rpc Uppercase(EchoRequest) returns (EchoResponse);
}
```

方法名既用于生成 C++ 接口，也会被 Tudou 写入 `CallHead.method_name`。服务端 Router 根据 service name 找到 Service，再根据 method name 找到具体方法。因此，即使当前 Service 只有一个方法，方法名也不能省略。

## 步骤二：使用 `protoc` 生成 C++ 代码

使用谷歌官方 `protoc` 命令行工具编译：

```bash
protoc --cpp_out=. echo.proto
```

生成两个文件：

- `echo.pb.h`：声明 `EchoRequest`、`EchoResponse`、服务端基类 `EchoService` 和客户端代理 `EchoService_Stub`；
- `echo.pb.cc`：实现消息编解码、反射信息、Service 分发和 Stub 调用。

可以先建立这个对应关系：

| `.proto` 定义 | 生成的 C++ 类型 | 谁使用 |
|---|---|---|
| `message EchoRequest` | `EchoRequest` | 客户端填写，服务端读取 |
| `message EchoResponse` | `EchoResponse` | 服务端填写，客户端读取 |
| `service EchoService` | `EchoService` | 服务端继承并实现 |
| `service EchoService` | `EchoService_Stub` | 客户端创建并调用 |

## 步骤三：在 C++ 代码中使用 Protobuf

### 1. 数据的序列化与反序列化

```cpp
#include "echo.pb.h"
#include <iostream>
#include <stdexcept>

using namespace tudou::example;

void demo() {
    // A. 创建并填充 C++ 对象
    EchoRequest request;
    request.set_message("Hello Tudou RPC!");

    // B. 序列化为二进制字节流 (SerializeToString)
    std::string binaryData;
    if (!request.SerializeToString(&binaryData)) {
        throw std::runtime_error("serialize failed");
    }

    // C. 接收端反序列化还原为 C++ 对象 (ParseFromString)
    EchoRequest decodedRequest;
    if (!decodedRequest.ParseFromString(binaryData)) {
        throw std::runtime_error("invalid protobuf message");
    }

    std::cout << decodedRequest.message() << std::endl;
}
```

### 2. 在服务端实现 RPC 业务服务

服务端继承生成的 `EchoService`，并实现真正的业务方法：

```cpp
class EchoServiceImpl final : public tudou::example::EchoService {
public:
    void Echo(google::protobuf::RpcController*,
              const tudou::example::EchoRequest* request,
              tudou::example::EchoResponse* response,
              google::protobuf::Closure* done) override {
        response->set_message("Echo: " + request->message());
        done->Run();
    }
};
```

注册的是业务实现对象，而不是生成的抽象基类：

```cpp
tudou::rpc::binary::Server server("127.0.0.1", 8091, 2);
server.register_service(std::make_shared<EchoServiceImpl>());
server.start();
```

服务端必须继承 `EchoService`，因为它需要提供真正的 `Echo()` 业务逻辑。`done->Run()` 表示本次同步方法已经填写完 response，Router 可以继续序列化并发送响应。

`Echo()` 的四个参数分别承担不同职责：

| 参数 | 含义 | Tudou 当前用法 |
|---|---|---|
| `controller` | RPC 错误、取消等控制信息 | 当前未实现，客户端传 `nullptr` |
| `request` | 服务端只读的请求消息 | Router 反序列化后传入 |
| `response` | 服务端填写的响应消息 | Router 随后序列化并回传 |
| `done` | 通知框架本次调用已经完成 | 同步 Service 在返回前调用 `Run()` |

### 3. 客户端通过 Stub 调用远程方法

客户端不实现业务逻辑，也不继承 `EchoService`。它创建生成的 `EchoService_Stub`：

```cpp
tudou::rpc::binary::CoroutineChannel channel(loop, "127.0.0.1", 8091);
tudou::example::EchoService_Stub stub(&channel);

tudou::example::EchoRequest request;
tudou::example::EchoResponse response;
request.set_message("hello Tudou");

stub.Echo(nullptr, &request, &response, nullptr);
std::cout << response.message() << '\n';
```

`stub.Echo()` 看起来像本地函数调用，但 Stub 没有实现 Echo 业务。它把方法描述符、request 和 response 交给 `CoroutineChannel::CallMethod()`，由 Channel 编码并发送远程请求。

### 4. 序列化和反序列化隐藏在哪里

Protobuf 生成的 Stub 不直接负责网络和序列化，它只把类型安全的方法调用转交给 `RpcChannel`：

```cpp
void EchoService_Stub::Echo(...) {
    channel_->CallMethod(
        descriptor()->method(0),
        controller,
        request,
        response,
        done);
}
```

Tudou 的 `CoroutineChannel` 才负责把调用转换成线上字节。发送请求时，它分别序列化调用头和业务请求，再交给 `FrameCodec` 编码完整帧：

```cpp
CallHead head;
head.set_service_name(method->service()->full_name());
head.set_method_name(method->name());

const Frame frame(
    FrameType::Request,
    sequenceId,
    head.SerializeAsString(),
    request->SerializeAsString());

return FrameCodec::encode(frame);
```

收到响应帧后，Channel 根据 `sequenceId` 找到客户端原来传入的 response 对象，并把响应 body 反序列化进去：

```cpp
call.response->ParseFromString(frame.body);
```

因此客户端虽然只写了一行：

```cpp
stub.Echo(nullptr, &request, &response, nullptr);
```

底层实际完成了：

```text
EchoRequest C++ 对象
  → Protobuf 请求字节
  → RPC Frame
  → TCP 发送
  → RPC 响应 Frame
  → Protobuf 反序列化
  → 填写原来的 EchoResponse 对象
```

当 `stub.Echo()` 返回时，`response` 已经填好。Protobuf Stub 提供“像本地方法一样调用”的类型安全接口，Tudou `CoroutineChannel` 提供真正的序列化、帧协议和网络传输。

### 5. 从 Stub 到服务端实现的完整路径

```text
客户端业务代码
  → EchoService_Stub::Echo()
  → CoroutineChannel::CallMethod()
  → 发送 service_name + method_name + Protobuf request

服务端 Router
  → 找到 EchoService 和 Echo MethodDescriptor
  → google::protobuf::Service::CallMethod()
  → protoc 生成的分发代码
  → EchoServiceImpl::Echo()
  → 填写 Protobuf response 并 done->Run()
```

因此两端角色非常明确：

```text
EchoService      → 服务端需要实现的业务接口
EchoService_Stub → 客户端使用的远程代理
```

## 步骤四：Schema 怎样安全演进

上线后的字段编号属于协议的一部分，不能随意改动。删除字段后应保留其编号和名称：

```protobuf
message User {
    reserved 2;
    reserved "old_name";

    int64 id = 1;
    string name = 3;
}
```

面试时记住四条即可：

- 新增字段通常兼容旧版本，旧代码会跳过不认识的字段；
- 不要修改已发布字段的 Tag；
- 不要让新字段复用已删除字段的 Tag，使用 `reserved` 防止误用；
- 不要随意改成 wire type 不兼容的字段类型。

# Result — 结果

- **紧凑的二进制编码**：字段使用数字 Tag，整数可以使用 Varint；实际体积和速度收益取决于消息结构与数据分布；
- **可演进的 Schema**：不复用旧 Tag、保持字段类型兼容时，可以安全新增字段，旧版本会忽略不认识的字段；
- **框架解耦**：利用 Protobuf 生成的 `Service` 基类与反射（Reflection），Tudou RPC 框架底层可以零入侵地调度任何业务服务。

# 核心设计要点提炼


| 特性         | JSON                                 | Protobuf                             |
| :------------- | :------------------------------------- | :------------------------------------- |
| **数据格式** | 文本格式 (ASCII/UTF-8)               | 紧凑二进制格式 (Binary)              |
| **Key 传输** | 传输完整的 Key 字符串 (`"username"`) | 编码字段编号与 wire type             |
| **解析开销** | 需要文本词法解析                     | 二进制解码，通常更低                 |
| **类型检查** | 弱类型 / 运行时校验                  | 编译期强类型校验                     |
| **向后兼容** | 依赖业务约定                         | 通过 Tag 和 Schema 演进规则保证      |

# 面试核心问答总结

## Q1：为什么 Protobuf 比 JSON 体积小得多？

1. **紧凑字段头代替 Key 名**：JSON 必须把 `"username"` 完整发出去；Protobuf 只编码字段编号和 wire type；
2. **Varint 变长编码**：对于整数，普通的 `int32_t` 总是占 4 字节；而 Protobuf 使用 Varint 编码，对于较小的数字（如 `1` 或 `100`），只用 1 个字节存储，极大地压缩了内存。

## Q2：Protobuf 如何保证向后兼容性（版本升级字段）？

Protobuf 靠字段后面的 **Tag 编号** 进行标识。如果新版本添加字段 `= 2`，旧版本会忽略或保留自己不认识的字段。兼容性的前提是遵守 Schema 演进规则，例如不要复用已经删除的 Tag，也不要随意改成不兼容的字段类型。

## Q3：`service EchoService` 和 `rpc Echo` 有什么区别？

`EchoService` 是一组 RPC 方法的服务名，`Echo` 是其中一个具体方法名。线上调用需要同时携带二者，服务端先找到 Service，再找到 Method。完整方法名是 `tudou.example.EchoService.Echo`。

## Q4：为什么服务端要继承 Service，而客户端不需要？

服务端负责执行真正的业务逻辑，因此继承 `EchoService` 并重写 `Echo()`；客户端只负责发起远程调用，因此使用 `EchoService_Stub`。Stub 将看似普通的 `Echo()` 调用转交给 `CoroutineChannel`，不会在客户端执行服务端业务代码。

## Q5：`stub.Echo()` 会自己完成序列化和网络发送吗？

不会。生成的 Stub 只调用 `RpcChannel::CallMethod()`。在 Tudou 中，`CoroutineChannel` 将 request 序列化为 Protobuf 字节、编码 RPC Frame 并通过 Socket 发送；收到响应后，它再把 Frame body 反序列化到调用方传入的 response 对象。

## Q6：Protobuf 本身是不是一个完整 RPC 框架？

不是。Protobuf 提供消息编解码、Service/Stub 代码和反射描述，但不提供 Tudou 所需的 TCP 分帧、请求响应匹配和协程调度。Tudou 把 Protobuf 消息放进自定义 Frame，并用 Reactor 完成网络传输。

## Q7：RPC 方法中的四个参数分别有什么作用？

`controller` 承载错误和取消等控制信息，`request` 是只读请求，`response` 由服务端填写，`done` 表示调用完成。Tudou 当前没有实现 Controller，因此客户端传 `nullptr`；服务端是同步模型，必须在返回前调用 `done->Run()`。
