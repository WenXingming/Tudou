// ============================================================================ //
// Router.cpp
// HTTP 路由器实现，严格按精确匹配、405 判定、前缀兜底、404 回退的顺序分发。
// ============================================================================ //

#include "Router.h"

#include <algorithm>
#include <sstream>

bool RouteKey::operator==(const RouteKey& other) const {
    return method == other.method && path == other.path;
}

std::size_t RouteKeyHash::operator()(const RouteKey& key) const {
    const std::size_t methodHash = std::hash<std::string>{}(key.method);
    const std::size_t pathHash = std::hash<std::string>{}(key.path);
    return methodHash ^ (pathHash + 0x9e3779b97f4a7c15ULL + (methodHash << 6) + (methodHash >> 2));
}

Router::Router() = default;

Router::~Router() = default;

DispatchResult Router::dispatch(const HttpRequest& req, HttpResponse& resp) const {
    // 先命中最具体的精确路由，避免兜底规则提前吞掉明确业务入口。
    const Handler* exactHandler = find_exact_handler(req);
    if (exactHandler != nullptr) {
        execute_handler(*exactHandler, req, resp);
        return DispatchResult::Matched;
    }

    // 同一路径存在但方法不匹配时，必须在任何兜底前明确产出 405 契约。
    const AllowedMethods* allowedMethods = find_allowed_methods(req.get_path());
    if (allowedMethods != nullptr) {
        write_method_not_allowed_response(req, *allowedMethods, resp);
        return DispatchResult::MethodNotAllowed;
    }

    // 只有不存在精确路由且不存在 405 分支时，前缀兜底才有资格接管请求。
    const Handler* prefixHandler = find_prefix_handler(req.get_path());
    if (prefixHandler != nullptr) {
        execute_handler(*prefixHandler, req, resp);
        return DispatchResult::Matched;
    }

    // 走到这里说明整个路由表都不认识该路径，应返回标准 404。
    write_not_found_response(req, resp);
    return DispatchResult::NotFound;
}

void Router::add_route(const std::string& method, const std::string& path, Handler handler) {
    // 精确路由表与允许方法索引必须同步维护，否则 405 分支会丢失 Allow 契约。
    exactRoutes_[RouteKey{ method, path }] = std::move(handler);
    allowedMethodsByPath_[path].insert(method);
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
    prefixRoutes_.push_back(PrefixRoute{ prefix, std::move(handler) });
}

void Router::set_not_found_handler(Handler handler) {
    notFoundHandler_ = std::move(handler);
}

void Router::set_method_not_allowed_handler(Handler handler) {
    methodNotAllowedHandler_ = std::move(handler);
}

const Router::Handler* Router::find_exact_handler(const HttpRequest& req) const {
    // method + path 是路由器的最小判定单元，先查精确表可以避免额外分支计算。
    const auto routeIt = exactRoutes_.find(RouteKey{ req.get_method(), req.get_path() });
    if (routeIt == exactRoutes_.end()) {
        return nullptr;
    }
    return &routeIt->second;
}

void Router::execute_handler(const Handler& handler, const HttpRequest& req, HttpResponse& resp) const {
    // 路由器统一通过这一跳执行外部注入的处理器，保持 dispatch 只负责流程编排。
    handler(req, resp);
}

const Router::AllowedMethods* Router::find_allowed_methods(const std::string& path) const {
    // 单独维护路径索引，是为了把“路径不存在”和“方法不允许”严格区分开。
    const auto methodsIt = allowedMethodsByPath_.find(path);
    if (methodsIt == allowedMethodsByPath_.end()) {
        return nullptr;
    }
    return &methodsIt->second;
}

void Router::write_method_not_allowed_response(
    const HttpRequest& req,
    const AllowedMethods& allowedMethods,
    HttpResponse& resp) const {
    // 自定义 405 处理器优先，允许上层系统覆盖默认文本响应但不改变分支语义。
    if (methodNotAllowedHandler_) {
        execute_handler(methodNotAllowedHandler_, req, resp);
        return;
    }

    // 没有覆盖处理器时，回落到路由器内建的最小 405 契约。
    fill_default_method_not_allowed_response(allowedMethods, resp);
}

void Router::fill_default_method_not_allowed_response(
    const AllowedMethods& allowedMethods,
    HttpResponse& resp) const {
    // 先构造统一的纯文本骨架，再按 405 语义补充 Allow 头，避免模板散落到多个分支。
    fill_plain_text_response(405, "Method Not Allowed", "Method Not Allowed", resp);

    const std::string allowHeader = format_allow_header(allowedMethods);
    if (!allowHeader.empty()) {
        resp.add_header("Allow", allowHeader);
    }
}

void Router::fill_plain_text_response(
    int statusCode,
    const std::string& reasonPhrase,
    const std::string& body,
    HttpResponse& resp) {
    // 404 与 405 共用同一份纯文本响应模板，保证默认错误响应契约一致。
    resp.set_http_version("HTTP/1.1");
    resp.set_status(statusCode, reasonPhrase);
    resp.set_body(body);
    resp.add_header("Content-Type", "text/plain");
    resp.add_header("Content-Length", std::to_string(body.size()));
    resp.set_close_connection(true);
}

std::string Router::format_allow_header(const AllowedMethods& allowedMethods) const {
    if (allowedMethods.empty()) {
        return "";
    }

    // 对方法名排序，保证测试、日志与抓包输出都具备稳定的可比性。
    std::vector<std::string> sortedMethods;
    sortedMethods.reserve(allowedMethods.size());
    for (const std::string& method : allowedMethods) {
        sortedMethods.push_back(method);
    }
    std::sort(sortedMethods.begin(), sortedMethods.end());

    std::ostringstream oss;
    for (std::size_t index = 0; index < sortedMethods.size(); ++index) {
        if (index > 0) {
            oss << ", ";
        }
        oss << sortedMethods[index];
    }
    return oss.str();
}

const Router::Handler* Router::find_prefix_handler(const std::string& path) const {
    // 前缀兜底的优先级等于注册顺序，因此这里必须保持线性扫描。
    for (const PrefixRoute& prefixRoute : prefixRoutes_) {
        if (starts_with(path, prefixRoute.prefix)) {
            return &prefixRoute.handler;
        }
    }
    return nullptr;
}

bool Router::starts_with(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    return text.compare(0, prefix.size(), prefix) == 0;
}

void Router::write_not_found_response(const HttpRequest& req, HttpResponse& resp) const {
    // 自定义 404 处理器只接管响应内容，不改变 dispatch 对未命中分支的判定。
    if (notFoundHandler_) {
        execute_handler(notFoundHandler_, req, resp);
        return;
    }

    // 默认 404 保持最小责任，只输出缺省响应契约。
    fill_default_not_found_response(resp);
}

void Router::fill_default_not_found_response(HttpResponse& resp) const {
    fill_plain_text_response(404, "Not Found", "Not Found", resp);
}
