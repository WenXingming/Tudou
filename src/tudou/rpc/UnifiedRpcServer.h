// ============================================================================
// UnifiedRpcServer 将同一个同步 Protobuf Service 暴露为二进制 RPC 与 JSON-RPC。
// 二进制调用交给 binary::Server，JSON/Protobuf 转换复用 binary::Router。
// ============================================================================

#pragma once

#include <memory>
#include <string>
#include <thread>

#include <google/protobuf/service.h>
#include <nlohmann/json.hpp>

#include "tudou/rpc/binary/Router.h"
#include "tudou/rpc/binary/Server.h"
#include "tudou/rpc/json/Server.h"

namespace tudou {
namespace rpc {

class UnifiedRpcServer {
public:
    UnifiedRpcServer(
        const std::string& ip,
        uint16_t binaryPort,
        uint16_t jsonPort,
        int numThreads = 0);
    ~UnifiedRpcServer();

    UnifiedRpcServer(const UnifiedRpcServer&) = delete;
    UnifiedRpcServer& operator=(const UnifiedRpcServer&) = delete;

    void register_service(std::shared_ptr<google::protobuf::Service> service);

    void start();
    void stop();

    uint16_t get_binary_port() const;
    uint16_t get_json_port() const;

private:
    void register_json_method(
        const std::shared_ptr<google::protobuf::Service>& service,
        const google::protobuf::MethodDescriptor* method);
    nlohmann::json invoke_json_method(
        google::protobuf::Service& service,
        const google::protobuf::MethodDescriptor& method,
        const nlohmann::json& params) const;

private:
    binary::Server binaryServer_;
    JsonRpcServer jsonServer_;
    binary::Router jsonBridgeRouter_;

    std::thread binaryThread_;
};

} // namespace rpc
} // namespace tudou
