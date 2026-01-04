/**
 * @file Router.cpp
 * @brief Minimal HTTP router: method + path -> handler 分发
 */

#include "Router.h"

#include <sstream>

void Router::add_route(const std::string& method, const std::string& path, Handler handler) {
    // 精确匹配的路由：method+path 为 key
    RouteKey key{method, path};
    routes_[key] = std::move(handler);

    // 同时记录“这个 path 支持哪些方法”，用于后续生成 405 的 Allow。
    // 这一步并不是为了加速精确匹配（精确匹配只依赖 routes_），而是为了更符合 HTTP 语义。
    methods_by_path_[path].insert(method);
}

void Router::add_get_route(const std::string& path, Handler handler) {
    add_route("GET", path, std::move(handler));
}

void Router::add_post_route(const std::string& path, Handler handler) {
    add_route("POST", path, std::move(handler));
}

void Router::add_head_route(const std::string& path, Handler handler) {
    add_route("HEAD", path, std::move(handler));
}

void Router::add_prefix_route(const std::string& prefix, Handler handler) {
    // 用于静态文件等兜底：按 path 前缀匹配
    // 重要：dispatch 会按 push_back 的顺序尝试。
    // 所以一般把更具体的前缀先注册，例如：
    //   "/static/" 先
    //   "/" 最后
    prefix_routes_.push_back({prefix, std::move(handler)});
}

void Router::set_not_found_handler(Handler handler) { not_found_handler_ = std::move(handler); }

void Router::set_method_not_allowed_handler(Handler handler) {
    method_not_allowed_handler_ = std::move(handler);
}

Router::DispatchResult Router::dispatch(const HttpRequest& req, HttpResponse& resp) const {
    // 1) 尝试精确匹配
    RouteKey key{req.get_method(), req.get_path()};
    auto it = routes_.find(key);
    if (it != routes_.end()) {
        // 命中：直接调用 handler 让它填充 resp
        auto hander = it->second;
        hander(req, resp);
        return DispatchResult::Matched;
    }

    // 2) 路径存在但方法不匹配 -> 405（更改为优先于前缀兜底）
    // 注意：这里必须基于“path 是否存在”来判断 405。
    // 如果某个 path 从未注册过任何 method，那么它应该是 404，而不是 405。
    auto methodIt = methods_by_path_.find(req.get_path());
    if (methodIt != methods_by_path_.end()) {
        if (method_not_allowed_handler_) {
            method_not_allowed_handler_(req, resp);
        }
        else {
            fill_default_method_not_allowed(req.get_path(), resp);
        }
        return DispatchResult::MethodNotAllowed;
    }

    // 3) 尝试前缀兜底（按注册顺序）。通常做静态文件分发处理。
    for (const auto& pr : prefix_routes_) {
        if (starts_with(req.get_path(), pr.first)) {
            // 命中前缀：通常是静态文件处理器或“所有 GET 请求的统一入口”（特殊的 Get 由精确路由处理）
            pr.second(req, resp);
            return DispatchResult::Matched;
        }
    }

    // 4) 路径不存在 -> 404
    if (not_found_handler_) {
        not_found_handler_(req, resp);
    } else {
        fill_default_not_found(resp);
    }
    return DispatchResult::NotFound;
}

// 私有工具函数，判断 text 是否以 prefix 开头
bool Router::starts_with(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    return text.compare(0, prefix.size(), prefix) == 0;
}

// 私有工具函数，填充默认 404 响应
void Router::fill_default_not_found(HttpResponse& resp) const {
    // 默认 404 响应（纯文本，关闭连接）
    resp.set_http_version("HTTP/1.1");
    resp.set_status(404, "Not Found");
    resp.set_body("Not Found");
    resp.add_header("Content-Type", "text/plain");
    resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
    resp.set_close_connection(true);
}

// 私有工具函数，填充默认 405 响应
void Router::fill_default_method_not_allowed(const std::string& path, HttpResponse& resp) const {
    // 默认 405 响应，自动生成 Allow 头
    resp.set_http_version("HTTP/1.1");
    resp.set_status(405, "Method Not Allowed");

    // Allow：告诉客户端“同一路径支持哪些方法”。
    // 例如：Allow: GET, POST
    resp.add_header("Allow", build_allow_header(path));
    resp.add_header("Content-Type", "text/plain");
    resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
    resp.set_body("Method Not Allowed");
    resp.set_close_connection(true);
}

// 私有工具函数，返回某 path 支持的所有方法，格式化为 Allow 头的值
std::string Router::build_allow_header(const std::string& path) const {
    auto it = methods_by_path_.find(path);
    if (it == methods_by_path_.end() || it->second.empty()) {
        return "";
    }

    std::ostringstream oss;
    bool first = true;
    for (const auto& method : it->second) {
        // HTTP 头通常用逗号+空格分隔多个值。
        // 如果不是第一个，就在前面加上逗号和空格。
        if (!first) {
            oss << ", ";
        }
        first = false;
        oss << method;
    }
    return oss.str();
}
