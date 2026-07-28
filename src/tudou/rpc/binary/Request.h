// ============================================================================
// Request 是 Server 从完整 Frame 中提取、交给 Router 的逻辑调用。
// 它只包含目标方法和 Protobuf 请求体，不携带帧序列号等传输状态。
// ============================================================================

#pragma once

#include <string>
#include <utility>

namespace tudou {
namespace rpc {
namespace binary {

struct Request {
    Request() : serviceName(), methodName(), body() {
    }

    Request(std::string requestServiceName, std::string requestMethodName, std::string requestBody)
        : serviceName(std::move(requestServiceName))
        , methodName(std::move(requestMethodName))
        , body(std::move(requestBody)) {
    }

    std::string serviceName;
    std::string methodName;
    std::string body;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
