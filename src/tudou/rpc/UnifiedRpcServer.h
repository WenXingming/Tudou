// ============================================================================
// UnifiedRpcServer 将同一个同步 Protobuf Service 暴露为二进制 RPC 与 JSON-RPC。
// 二进制请求交给 binary::Server，JSON 请求转换为 Protobuf 后直接调用 Service。
// ============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <google/protobuf/service.h>

#include "tudou/rpc/binary/Server.h"
#include "tudou/rpc/json/Server.h"

namespace tudou {
namespace rpc {

class UnifiedRpcServer {
public:
    UnifiedRpcServer(const std::string& ip, uint16_t binaryPort, uint16_t jsonPort, int numThreads = 0);
    ~UnifiedRpcServer();

    UnifiedRpcServer(const UnifiedRpcServer&) = delete;
    UnifiedRpcServer& operator=(const UnifiedRpcServer&) = delete;

    // 必须在 start() 前注册；同一 Service 可能被两个协议的 IO 线程并发调用。
    void register_service(std::shared_ptr<google::protobuf::Service> service);

    void start();
    void stop();

    uint16_t get_binary_port() const;
    uint16_t get_json_port() const;

private:
    binary::Server binaryServer_;
    JsonRpcServer jsonServer_;

    std::thread binaryThread_; // 两个 Server::start() 都会阻塞，二进制服务在内部线程运行。
};

} // namespace rpc
} // namespace tudou
