// ============================================================================
// HTTP 路由器：按 method + path 分发请求，并提供前缀路由与 404 回退。
// 只负责路由匹配和响应构建，不负责 HTTP 解析、连接管理或网络发送。
// ============================================================================

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"

// 精确路由键：method + path 一起决定一个唯一处理器。
struct RouteKey {
    std::string method;
    std::string path;

    bool operator==(const RouteKey& other) const;
};

// 为 RouteKey 提供 unordered_map 所需的哈希策略。
struct RouteKeyHash {
    std::size_t operator()(const RouteKey& key) const;
};

class HttpRouter {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

    HttpRouter();
    ~HttpRouter();

    void dispatch(const HttpRequest& req, HttpResponse& resp) const; // 路由分发总入口。

    void add_route(const std::string& method, const std::string& path, Handler handler);
    void add_get_route(const std::string& path, Handler handler);
    void add_post_route(const std::string& path, Handler handler);
    void add_head_route(const std::string& path, Handler handler);
    void add_prefix_route(const std::string& prefix, Handler handler); // 注册前缀兜底路由。

private:
    struct PrefixRoute {
        std::string prefix;
        Handler handler;
    };

    const Handler* find_exact_handler(const HttpRequest& req) const;
    const Handler* find_prefix_handler(const std::string& path) const;

private:
    std::unordered_map<RouteKey, Handler, RouteKeyHash> exactRoutes_;  // 精确路由表，使用 method + path 锁定唯一处理器。
    std::vector<PrefixRoute> prefixRoutes_;                            // 前缀兜底路由表，保持注册顺序表达匹配优先级（所以使用 vector 而不是 map）。
};
