/**
 * @file BinaryRpcRouter.cpp
 * @brief 基于 Protobuf 运行时反射机制的动态 RPC 路由器实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "BinaryRpcRouter.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <memory>

namespace tudou {
namespace rpc {
namespace binary {

// 自定义包装的 Closure 闭包，用于管理异步回调 and response 的生命周期自动回收
class BinaryRpcClosure : public google::protobuf::Closure {
public:
    BinaryRpcClosure(google::protobuf::Message* response,
                     std::function<void(const std::string&)> callback)
        : response_(response)
        , callback_(std::move(callback)) {
    }

    ~BinaryRpcClosure() override = default;

    void Run() override {
        // 使用 RAII 智能指针接管，确保退出函数作用域时自动执行销毁，防止内存泄漏
        std::unique_ptr<google::protobuf::Closure> selfGuard(this);
        std::unique_ptr<google::protobuf::Message> responseGuard(response_);

        std::string responseRaw;
        if (responseGuard && responseGuard->SerializeToString(&responseRaw)) {
            callback_(responseRaw);
        }
        else {
            spdlog::error("BinaryRpcRouter: Failed to serialize response payload");
            callback_("");
        }
    }

private:
    google::protobuf::Message* response_;
    std::function<void(const std::string&)> callback_;
};

BinaryRpcRouter::BinaryRpcRouter() = default;
BinaryRpcRouter::~BinaryRpcRouter() = default;

void BinaryRpcRouter::register_service(std::shared_ptr<google::protobuf::Service> service) {
    if (!service) {
        spdlog::error("BinaryRpcRouter: Attempted to register a null service");
        return;
    }

    // 反射：运行时动态获取服务名
    const google::protobuf::ServiceDescriptor* descriptor = service->GetDescriptor();
    std::string serviceName = descriptor->full_name();

    ServiceInfo serviceInfo;
    serviceInfo.service = service;

    // 反射：动态获取所有方法。遍历服务下的所有远程方法并做映射缓存
    int methodCount = descriptor->method_count();
    for (int index = 0; index < methodCount; ++index) {
        const google::protobuf::MethodDescriptor* method = descriptor->method(index);
        serviceInfo.methods[method->name()] = method;
        spdlog::info("BinaryRpcRouter: Method registered, method={}.{}", serviceName, method->name());
    }

    services_[serviceName] = std::move(serviceInfo);
    spdlog::info("BinaryRpcRouter: Service registered successfully, name={}", serviceName);
}

void BinaryRpcRouter::dispatch(const std::string& serviceName,
                               const std::string& methodName,
                               const std::string& requestRaw,
                               std::function<void(const std::string& responseRaw)> doneCallback) {
    // 1. 查找服务对象
    auto serviceIt = services_.find(serviceName);
    if (serviceIt == services_.end()) {
        spdlog::warn("BinaryRpcRouter: Service not found, name={}", serviceName);
        throw std::invalid_argument("Service not found: " + serviceName);
    }

    const ServiceInfo& serviceInfo = serviceIt->second;

    // 2. 查找方法描述符
    auto methodIt = serviceInfo.methods.find(methodName);
    if (methodIt == serviceInfo.methods.end()) {
        spdlog::warn("BinaryRpcRouter: Method not found in service {}, name={}", serviceName, methodName);
        throw std::invalid_argument("Method not found: " + serviceName + "." + methodName);
    }

    const google::protobuf::MethodDescriptor* method = methodIt->second;

    // 3. 动态构建请求消息实例
    std::unique_ptr<google::protobuf::Message> request(serviceInfo.service->GetRequestPrototype(method).New());
    if (!request->ParseFromString(requestRaw)) {
        spdlog::error("BinaryRpcRouter: Failed to parse request payload for method={}.{}", serviceName, methodName);
        throw std::invalid_argument("Invalid request payload for method: " + serviceName + "." + methodName);
    }

    // 4. 动态构建响应消息实例
    // 生命周期移交给 done 闭包持有，由 BinaryRpcClosure::Run() 析构时自动回收
    auto* response = serviceInfo.service->GetResponsePrototype(method).New();

    // 5. 实例化 Closure 闭包
    auto* done = new BinaryRpcClosure(response, std::move(doneCallback));

    // 6. 执行具体的 RPC 业务分发
    try {
        serviceInfo.service->CallMethod(method, nullptr, request.get(), response, done);
    }
    catch (const std::exception& e) {
        spdlog::error("BinaryRpcRouter: Exception caught during CallMethod, error={}", e.what());
        // 异常分支下，由于 Run() 无法被执行，需在此处手动销毁 done，防范内存泄漏
        delete done;
        throw;
    }
}

} // namespace binary
} // namespace rpc
} // namespace tudou
