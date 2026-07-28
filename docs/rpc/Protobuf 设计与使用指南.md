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

1. **数据序列化与反序列化**：将 C++ 业务对象压缩为极小的二进制字节流（`SerializeToString`），并在接收端安全还原（`ParseFromString`）；
2. **RPC 接口定义语言 (IDL)**：使用 `.proto` 文件统一声明 Service 接口与数据 Message，并自动生成 C++ 虚函数基类与客户端 Stub。

# Action — 设计与使用工作流

## 工作流全景图

```text
  ┌─────────────────────────┐
  │   1. 编写 test.proto    │  (定义消息 Message 与 Service 接口)
  └────────────┬────────────┘
               │ protoc 编译器编译
               ▼
  ┌─────────────────────────┐
  │ 2. 生成 test.pb.h / cc  │  (自动生成 C++ 类与 Service 虚基类)
  └────────────┬────────────┘
               │ 业务代码继承与调用
               ▼
  ┌─────────────────────────┐
  │ 3. 业务代码与 RPC 管道  │  (调用 SerializeToString / CallMethod)
  └─────────────────────────┘
```

## 步骤一：编写 `.proto` 文件

在 [`test/unitTest/tudou/rpc/test.proto`](file:///home/wxm/Tudou/test/unitTest/tudou/rpc/test.proto) 中：

```protobuf
syntax = "proto3";

package tudou.rpc.test;

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
service TestEchoService {
    rpc Echo(EchoRequest) returns (EchoResponse);
}
```

### 关键语法点：

- **Tag 编号 (`= 1`, `= 2`)**：二进制传输时，Protobuf 不会发送 `"message"` 这个字符串名字，而是仅仅发送 Tag 数字 `1`！这是 Protobuf 体积远小于 JSON 的核心秘密。
- **`option cc_generic_services = true;`**：指示 `protoc` 生成继承自 `google::protobuf::Service` 的 C++ RPC 虚基类与 Stub。

## 步骤二：使用 `protoc` 生成 C++ 代码

使用谷歌官方 `protoc` 命令行工具编译：

```bash
protoc --cpp_out=. test.proto
```

生成两个文件：

- `test.pb.h`：包含 C++ 类 `EchoRequest`、`EchoResponse` 以及 `TestEchoService` 的声明；
- `test.pb.cc`：包含对应类的序列化/反序列化与反射实现。

## 步骤三：在 C++ 代码中使用 Protobuf

### 1. 数据的序列化与反序列化

```cpp
#include "test.pb.h"
#include <iostream>

using namespace tudou::rpc::test;

void demo() {
    // A. 创建并填充 C++ 对象
    EchoRequest request;
    request.set_message("Hello Tudou RPC!");

    // B. 序列化为二进制字节流 (SerializeToString)
    std::string binaryData;
    request.SerializeToString(&binaryData);

    // C. 接收端反序列化还原为 C++ 对象 (ParseFromString)
    EchoResponse newResponse;
    newResponse.ParseFromString(binaryData);

    std::cout << newResponse.message() << std::endl;
}
```

### 2. 在服务端实现 RPC 业务服务

服务端继承 `TestEchoService` 并重写对应的纯虚函数：

```cpp
class TestEchoServiceImpl : public TestEchoService {
public:
    void Echo(google::protobuf::RpcController* controller,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) override {
        // 实现具体的业务逻辑
        response->set_message("Echo: " + request->message());

        // 完成后调用 done 闭包通知框架
        if (done) {
            done->Run();
        }
    }
};
```

# Result — 结果

- **极极致性能**：体积比 JSON 小 3~10 倍，序列化/反序列化速度快 20~100 倍；
- **完美的向后兼容性 (Schema Evolution)**：只要字段的 Tag 编号（如 `= 1`）不变，哪怕未来新增了字段，旧版本的客户端依然能正常解析新版本发出的数据包（未识别的 Tag 自动忽略）；
- **框架解耦**：利用 Protobuf 生成的 `Service` 基类与反射（Reflection），Tudou RPC 框架底层可以零入侵地调度任何业务服务。

# 核心设计要点提炼


| 特性         | JSON                                 | Protobuf                             |
| :------------- | :------------------------------------- | :------------------------------------- |
| **数据格式** | 文本格式 (ASCII/UTF-8)               | 紧凑二进制格式 (Binary)              |
| **Key 传输** | 传输完整的 Key 字符串 (`"username"`) | 只传输数字 Tag 编号 (`1`)            |
| **解析开销** | 高（复杂字符串查找与词法解析）       | 极低（直接按 Offset 与 Varint 解码） |
| **类型检查** | 弱类型 / 运行时校验                  | 编译期强类型校验                     |
| **向后兼容** | 容易                                 | 极佳（靠 Tag 编号维持版本兼容）      |

# 面试核心问答总结

## Q1：为什么 Protobuf 比 JSON 体积小得多？

1. **Tag 代替 Key 名**：JSON 必须把每个字段名 `"username"` 完整发出去；Protobuf 在传输时只发送数字 Tag 编号（如 `1`）；
2. **Varint 变长编码**：对于整数，普通的 `int32_t` 总是占 4 字节；而 Protobuf 使用 Varint 编码，对于较小的数字（如 `1` 或 `100`），只用 1 个字节存储，极大地压缩了内存。

## Q2：Protobuf 如何保证向后兼容性（版本升级字段）？

Protobuf 靠字段后面的 **Tag 编号** 进行标识。如果新版本添加了字段 `= 2`，而旧版本的服务端收到了这个包，旧版本读取到 Tag `2` 时由于不认识该字段，会自动将其忽略或放入未识别字段列表中，程序绝对不会崩掉或解析错位。
