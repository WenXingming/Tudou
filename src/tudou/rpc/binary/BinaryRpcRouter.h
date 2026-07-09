/**
 * @file BinaryRpcRouter.h
 * @brief 基于 Protobuf 运行时反射机制的动态 RPC 路由器声明
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <google/protobuf/service.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>

namespace tudou {
namespace rpc {
namespace binary {

class BinaryRpcRouter {
public:
    BinaryRpcRouter();
    ~BinaryRpcRouter();

    // 禁用拷贝构造和赋值
    BinaryRpcRouter(const BinaryRpcRouter&) = delete;
    BinaryRpcRouter& operator=(const BinaryRpcRouter&) = delete;

    /**
     * @brief 注册一个具体的 Protobuf RPC 业务服务对象。
     *        内部会自动反射提取该服务声明的全部远程调用方法。
     * @param service 继承自 google::protobuf::Service 的具体服务实例指针。
     */
    void register_service(std::shared_ptr<google::protobuf::Service> service);

    /**
     * @brief 解析二进制载荷，并动态反射调度具体的业务方法。
     * @param serviceName 服务名称，如 "tudou.rpc.UserService"
     * @param methodName 方法名称，如 "Login"
     * @param requestRaw 序列化后的 Request 二进制字节流
     * @param doneCallback 执行完成后的回调闭包。参数为序列化后的 Response 二进制。
     */
    void dispatch(const std::string& serviceName,
                  const std::string& methodName,
                  const std::string& requestRaw,
                  std::function<void(const std::string& responseRaw)> doneCallback);

private:
    struct ServiceInfo {
        std::shared_ptr<google::protobuf::Service> service;
        std::unordered_map<std::string, const google::protobuf::MethodDescriptor*> methods;
    };

    // 映射: ServiceName -> ServiceInfo
    std::unordered_map<std::string, ServiceInfo> services_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
