// ============================================================================
// JSON-RPC 2.0 消息路由器与协议分发器。
// 负责方法回调注册、JSON-RPC 请求校验、单请求/批量请求路由分发与错误响应构造。
// ============================================================================

#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace tudou {
namespace rpc {

enum class JsonRpcErrorCode {
    ParseError     = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams  = -32602,
    InternalError  = -32603
};

class JsonRpcRouter {
public:
    using RpcHandler = std::function<nlohmann::json(const nlohmann::json& params)>;

    JsonRpcRouter();
    ~JsonRpcRouter();

    JsonRpcRouter(const JsonRpcRouter&) = delete;
    JsonRpcRouter& operator=(const JsonRpcRouter&) = delete;

    // 注册业务方法处理器（如 "add", "UserService.login"）。
    void register_method(const std::string& name, RpcHandler handler);

    // 解析并分发网络 JSON 请求文本（支持单请求、Notification 以及 Batch 批量请求）。
    std::string dispatch(const std::string& requestStr);

private:
    std::string dispatch_batch(const nlohmann::json& request);
    nlohmann::json dispatch_single(const nlohmann::json& req);

    static nlohmann::json make_success_response(const nlohmann::json& id, nlohmann::json result);
    static nlohmann::json make_error_response(const nlohmann::json& id, JsonRpcErrorCode code, const std::string& message);

private:
    std::unordered_map<std::string, RpcHandler> methods_;
};

} // namespace rpc
} // namespace tudou
