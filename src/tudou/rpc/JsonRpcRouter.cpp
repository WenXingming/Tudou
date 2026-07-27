// ============================================================================
// JSON-RPC 2.0 消息路由器与协议分发器实现。
// ============================================================================

#include "JsonRpcRouter.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

JsonRpcRouter::JsonRpcRouter() = default;
JsonRpcRouter::~JsonRpcRouter() = default;

void JsonRpcRouter::register_method(const std::string& name, RpcHandler handler) {
    methods_[name] = std::move(handler);
    spdlog::info("JsonRpcRouter: Method registered successfully, name={}", name);
}

std::string JsonRpcRouter::dispatch(const std::string& requestStr) {
    if (requestStr.empty()) {
        return make_error_response(nullptr, JsonRpcErrorCode::InvalidRequest, "Invalid Request (empty body)").dump();
    }

    // 尝试 JSON 语法解析，解析失败返回 -32700 Parse error
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(requestStr);
    }
    catch (const nlohmann::json::parse_error& e) {
        spdlog::error("JsonRpcRouter: JSON parse failed, error={}", e.what());
        return make_error_response(nullptr, JsonRpcErrorCode::ParseError, "Parse error").dump();
    }

    // 区分 Batch 批量请求与 Single 单请求分发
    if (request.is_array()) {
        return dispatch_batch(request);
    }
    nlohmann::json singleResponse = dispatch_single(request);
    return singleResponse.is_null() ? "" : singleResponse.dump();
}

std::string JsonRpcRouter::dispatch_batch(const nlohmann::json& request) {
    if (request.empty()) {
        return make_error_response(nullptr, JsonRpcErrorCode::InvalidRequest, "Invalid Request (empty batch)").dump();
    }

    nlohmann::json batchResponse = nlohmann::json::array();
    for (const auto& req : request) {
        nlohmann::json singleResponse = dispatch_single(req);
        if (singleResponse.is_null()) {
            spdlog::debug("JsonRpcRouter: Batch request contains Notification, no response generated for this item");
            continue;
        }
        batchResponse.push_back(std::move(singleResponse));
    }

    // 若批量请求中全为 Notification，按规范无需产生响应
    if (batchResponse.empty()) {
        return "";
    }
    return batchResponse.dump();
}

nlohmann::json JsonRpcRouter::dispatch_single(const nlohmann::json& req) {
    // 必须是 Object 字典
    if (!req.is_object()) {
        return make_error_response(nullptr, JsonRpcErrorCode::InvalidRequest, "Invalid Request (not an object)");
    }

    // 识别 Notification（缺少 id 字段时为通知请求，无需回复）
    bool isNotification = true;
    nlohmann::json id = nullptr;
    if (req.contains("id")) {
        id = req["id"];
        isNotification = false;
        if (!id.is_string() && !id.is_number() && !id.is_null()) {
            return make_error_response(nullptr, JsonRpcErrorCode::InvalidRequest, "Invalid Request (id must be string, number or null)");
        }
    }

    // 校验 jsonrpc 版本及 method 方法名声明
    if (!req.contains("jsonrpc") || req["jsonrpc"] != "2.0") {
        return make_error_response(id, JsonRpcErrorCode::InvalidRequest, "Invalid Request (missing or invalid jsonrpc version)");
    }

    if (!req.contains("method") || !req["method"].is_string()) {
        return make_error_response(id, JsonRpcErrorCode::InvalidRequest, "Invalid Request (missing or invalid method name)");
    }

    // 路由匹配
    std::string methodName = req["method"];
    auto it = methods_.find(methodName);
    if (it == methods_.end()) {
        spdlog::warn("JsonRpcRouter: Method not found, name={}", methodName);
        return make_error_response(id, JsonRpcErrorCode::MethodNotFound, "Method not found");
    }

    // 业务参数结构校验（必须为 object、array 或 null）
    nlohmann::json params = nullptr;
    if (req.contains("params")) {
        params = req["params"];
        if (!params.is_object() && !params.is_array() && !params.is_null()) {
            return make_error_response(id, JsonRpcErrorCode::InvalidParams, "Invalid params (must be structured object or array)");
        }
    }

    // 调度业务 Handler 执行并做异常分类映射
    nlohmann::json result;
    try {
        auto methodHandler = it->second;
        result = methodHandler(params);
    }
    catch (const nlohmann::json::exception& e) {
        spdlog::error("JsonRpcRouter: Invalid params exception for method={}, error={}", methodName, e.what());
        return make_error_response(id, JsonRpcErrorCode::InvalidParams, "Invalid params: " + std::string(e.what()));
    }
    catch (const std::invalid_argument& e) {
        spdlog::error("JsonRpcRouter: Invalid argument for method={}, error={}", methodName, e.what());
        return make_error_response(id, JsonRpcErrorCode::InvalidParams, "Invalid params: " + std::string(e.what()));
    }
    catch (const std::exception& e) {
        spdlog::error("JsonRpcRouter: Internal error for method={}, error={}", methodName, e.what());
        return make_error_response(id, JsonRpcErrorCode::InternalError, "Internal error: " + std::string(e.what()));
    }
    catch (...) {
        spdlog::error("JsonRpcRouter: Unknown internal exception for method={}", methodName);
        return make_error_response(id, JsonRpcErrorCode::InternalError, "Internal error: Unknown exception");
    }

    // Notification 无需产生响应消息；否则构造成功包
    if (isNotification) {
        return nullptr;
    }

    return make_success_response(id, std::move(result));
}

nlohmann::json JsonRpcRouter::make_success_response(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["result"] = std::move(result);
    resp["id"] = id.is_null() ? nullptr : id;
    return resp;
}

nlohmann::json JsonRpcRouter::make_error_response(const nlohmann::json& id, JsonRpcErrorCode code, const std::string& message) {
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";

    nlohmann::json err;
    err["code"] = static_cast<int>(code);
    err["message"] = message;

    resp["error"] = std::move(err);
    resp["id"] = id.is_null() ? nullptr : id;
    return resp;
}
