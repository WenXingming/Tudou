/**
 * @file JsonRpcRouter.cpp
 * @brief JSON-RPC 2.0 核心分发路由实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "JsonRpcRouter.h"

#include <exception>
#include <stdexcept>
#include <spdlog/spdlog.h>

JsonRpcRouter::JsonRpcRouter() = default;
JsonRpcRouter::~JsonRpcRouter() = default;

void JsonRpcRouter::register_method(const std::string& name, RpcHandler handler) {
    methods_[name] = std::move(handler);
    spdlog::info("JsonRpcRouter: Method registered successfully, name={}", name);
}

std::string JsonRpcRouter::dispatch(const std::string& requestStr) {
    if (requestStr.empty()) {
        return make_error_response(nullptr, -32600, "Invalid Request (empty body)").dump();
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(requestStr);
    }
    catch (const nlohmann::json::parse_error& e) {
        spdlog::error("JsonRpcRouter: JSON parse failed, error={}", e.what());
        return make_error_response(nullptr, -32700, "Parse error").dump();
    }

    // 处理批量请求 (Batch Requests)
    if (root.is_array()) {
        if (root.empty()) {
            return make_error_response(nullptr, -32600, "Invalid Request (empty batch)").dump();
        }

        nlohmann::json batchResponse = nlohmann::json::array();
        for (const auto& req : root) {
            nlohmann::json singleResponse = dispatch_single(req);
            if (!singleResponse.is_null()) {
                batchResponse.push_back(std::move(singleResponse));
            }
        }

        if (batchResponse.empty()) {
            return ""; // 若全部为 Notification，则无需任何响应
        }
        return batchResponse.dump();
    }

    // 处理单次请求
    nlohmann::json singleResponse = dispatch_single(root);
    if (singleResponse.is_null()) {
        return ""; // 单次 Notification 请求无需回复
    }
    return singleResponse.dump();
}

nlohmann::json JsonRpcRouter::dispatch_single(const nlohmann::json& req) {
    // 1. 基础的协议语法规范性校验
    if (!req.is_object()) {
        return make_error_response(nullptr, -32600, "Invalid Request (not an object)");
    }

    // 提取请求 id，如果没有该字段，则该请求属于 Notification
    nlohmann::json id = nullptr;
    bool isNotification = true;
    if (req.contains("id")) {
        id = req["id"];
        isNotification = false;
        // 校验 id 类型规范（必须是 string, number 或 null）
        if (!id.is_string() && !id.is_number() && !id.is_null()) {
            return make_error_response(nullptr, -32600, "Invalid Request (id must be string, number or null)");
        }
    }

    // 校验 "jsonrpc" 字段
    if (!req.contains("jsonrpc") || req["jsonrpc"] != "2.0") {
        return make_error_response(id, -32600, "Invalid Request (missing or invalid jsonrpc version)");
    }

    // 校验 "method" 字段
    if (!req.contains("method") || !req["method"].is_string()) {
        return make_error_response(id, -32600, "Invalid Request (missing or invalid method name)");
    }

    std::string methodName = req["method"];

    // 2. 匹配业务方法
    auto it = methods_.find(methodName);
    if (it == methods_.end()) {
        spdlog::warn("JsonRpcRouter: Method not found, name={}", methodName);
        return make_error_response(id, -32601, "Method not found");
    }

    // 提取参数
    nlohmann::json params = nullptr;
    if (req.contains("params")) {
        params = req["params"];
        // 校验 params 类型规范（必须是 object 或 array）
        if (!params.is_object() && !params.is_array() && !params.is_null()) {
            return make_error_response(id, -32602, "Invalid params (must be structured object or array)");
        }
    }

    // 3. 执行具体业务逻辑
    nlohmann::json result;
    try {
        result = it->second(params);
    }
    catch (const nlohmann::json::exception& e) {
        // 通常是 params 参数读取类型不匹配异常，归为 Invalid params 错误
        spdlog::error("JsonRpcRouter: Invalid params exception for method={}, error={}", methodName, e.what());
        return make_error_response(id, -32602, "Invalid params: " + std::string(e.what()));
    }
    catch (const std::invalid_argument& e) {
        spdlog::error("JsonRpcRouter: Invalid argument for method={}, error={}", methodName, e.what());
        return make_error_response(id, -32602, "Invalid params: " + std::string(e.what()));
    }
    catch (const std::exception& e) {
        // 服务端内部异常，归为 Internal error
        spdlog::error("JsonRpcRouter: Internal error for method={}, error={}", methodName, e.what());
        return make_error_response(id, -32603, "Internal error: " + std::string(e.what()));
    }
    catch (...) {
        spdlog::error("JsonRpcRouter: Unknown internal exception for method={}", methodName);
        return make_error_response(id, -32603, "Internal error: Unknown exception");
    }

    // 4. Notification 不需要生成任何返回包
    if (isNotification) {
        return nullptr;
    }

    // 5. 构造成功的响应包
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["result"] = std::move(result);
    resp["id"] = std::move(id);
    return resp;
}

nlohmann::json JsonRpcRouter::make_error_response(const nlohmann::json& id, int code, const std::string& message) {
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    
    nlohmann::json err;
    err["code"] = code;
    err["message"] = message;
    
    resp["error"] = std::move(err);
    resp["id"] = id.is_null() ? nullptr : id;
    return resp;
}
