// ============================================================================
// HTTP 路由器实现，按精确匹配、前缀兜底、404 回退的顺序分发。
// ============================================================================

#include "tudou/http/HttpRouter.h"

bool RouteKey::operator==(const RouteKey& other) const {
    return method == other.method && path == other.path;
}

std::size_t RouteKeyHash::operator()(const RouteKey& key) const {
    // 分别计算 method（如 "GET"）和 path（如 "/index.html"）的独立哈希值。
    const std::size_t methodHash = std::hash<std::string>{}(key.method);
    const std::size_t pathHash = std::hash<std::string>{}(key.path);

    // 哈希组合算法，参考 boost::hash_combine。
    return methodHash ^ (pathHash + 0x9e3779b97f4a7c15ULL + (methodHash << 6) + (methodHash >> 2));
}

HttpRouter::HttpRouter() = default;

HttpRouter::~HttpRouter() = default;

void HttpRouter::dispatch(const HttpRequest& req, HttpResponse& resp) const {
    // 先命中最具体的精确路由，避免兜底规则提前吞掉明确业务入口。
    const Handler* exactHandler = find_exact_handler(req);
    if (exactHandler != nullptr) {
        (*exactHandler)(req, resp);
        return;
    }

    // 精确路由未命中后，交给按注册顺序排列的前缀路由兜底。
    const Handler* prefixHandler = find_prefix_handler(req.get_path());
    if (prefixHandler != nullptr) {
        (*prefixHandler)(req, resp);
        return;
    }

    // 走到这里说明整个路由表都不认识该请求，应返回默认 404。
    resp = HttpResponse{};
    resp.set_status(404, "Not Found");
    resp.set_header("Content-Type", "text/plain");
    resp.set_body("Not Found");
    resp.set_header("Connection", "close");
}

void HttpRouter::add_route(const std::string& method, const std::string& path, Handler handler) {
    exactRoutes_[RouteKey{ method, path }] = std::move(handler);
}

void HttpRouter::add_get_route(const std::string& path, Handler handler) {
    add_route("GET", path, std::move(handler));
}

void HttpRouter::add_post_route(const std::string& path, Handler handler) {
    add_route("POST", path, std::move(handler));
}

void HttpRouter::add_head_route(const std::string& path, Handler handler) {
    add_route("HEAD", path, std::move(handler));
}

void HttpRouter::add_prefix_route(const std::string& prefix, Handler handler) {
    // 前缀路由依赖注册顺序表达优先级，因此这里保留线性容器。
    prefixRoutes_.push_back(PrefixRoute{ prefix, std::move(handler) });
}

const HttpRouter::Handler* HttpRouter::find_exact_handler(const HttpRequest& req) const {
    // method + path 是路由器的最小判定单元，先查精确表可以避免额外分支计算。
    const auto routeIt = exactRoutes_.find(RouteKey{ req.get_method(), req.get_path() });
    if (routeIt == exactRoutes_.end()) {
        return nullptr;
    }
    return &routeIt->second;
}

const HttpRouter::Handler* HttpRouter::find_prefix_handler(const std::string& path) const {
    // 前缀兜底的优先级等于注册顺序，因此这里必须保持线性扫描。
    for (const PrefixRoute& prefixRoute : prefixRoutes_) {
        if (path.size() >= prefixRoute.prefix.size()
            && path.compare(0, prefixRoute.prefix.size(), prefixRoute.prefix) == 0) {
            return &prefixRoute.handler;
        }
    }
    return nullptr;
}
