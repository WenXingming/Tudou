/**
 * @file JsonRpcRouter.h
 * @brief JSON-RPC 2.0 核心分发路由声明
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>

class JsonRpcRouter {
public:
    using RpcHandler = std::function<nlohmann::json(const nlohmann::json& params)>;

    JsonRpcRouter();
    ~JsonRpcRouter();

    // 禁用拷贝构造和赋值
    JsonRpcRouter(const JsonRpcRouter&) = delete;
    JsonRpcRouter& operator=(const JsonRpcRouter&) = delete;

    /**
     * @brief 注册具体的远程过程调用函数。
     * @param name 方法名称（如 "add", "userService.login" 等）
     * @param handler 业务方法处理器。接收 json 参数并返回执行结果。
     */
    void register_method(const std::string& name, RpcHandler handler);

    /**
     * @brief 解析并分发处理网络传来的 JSON 请求文本。
     * @param requestStr 请求文本字节流（支持单请求、Notification 以及 Batch 批量请求）。
     * @return 返回代表响应的 JSON 文本字符流。如果全是 Notification，则返回空字符串。
     */
    std::string dispatch(const std::string& requestStr);

private:
    nlohmann::json dispatch_single(const nlohmann::json& req);
    nlohmann::json make_error_response(const nlohmann::json& id, int code, const std::string& message);

    std::unordered_map<std::string, RpcHandler> methods_;
};
