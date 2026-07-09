// ============================================================================
// Router.h
// HTTP 路由器对外契约，负责精确路由、方法约束与前缀兜底分发。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// Router.h
// ├── RouteKey
// │   └── operator==(other) const                # [公有] 判断 method + path 是否完全一致
// ├── RouteKeyHash
// │   └── operator()(key) const                  # [公有] 为精确路由键生成哈希值
// └── Router
//     ├── Router()                               # [公有] 构造空路由表，等待后续注册路由
//     ├── ~Router()                              # [公有] 析构路由容器本身，不执行额外回收逻辑
//     ├── dispatch(req, resp) const              # [公有] 分发总入口：精确匹配 -> 405 -> 前缀兜底 -> 404
//     │   ├── find_exact_handler(req) const      # [私有] 先查 method + path 的精确路由
//     │   ├── find_allowed_methods(path) const   # [私有] 查询该路径允许的方法集合
//     │   ├── write_method_not_allowed_response(...) const  # [私有] 生成 405 响应
//     │   │   └── format_allow_header(...) const       # [私有] 生成 Allow 头
//     │   ├── find_prefix_handler(path) const    # [私有] 按注册顺序查前缀兜底路由
//     │   └── write_not_found_response(req, resp) const    # [私有] 生成 404 响应
//     ├── add_route(method, path, handler)       # [公有] 注册精确路由并同步允许方法索引
//     ├── add_get_route(path, handler)           # [公有] GET 精确路由快捷入口
//     ├── add_post_route(path, handler)          # [公有] POST 精确路由快捷入口
//     ├── add_head_route(path, handler)          # [公有] HEAD 精确路由快捷入口
//     ├── add_prefix_route(prefix, handler)      # [公有] 注册前缀兜底路由
//     ├── set_not_found_handler(handler)         # [公有] 注入自定义 404 处理器
//     └── set_method_not_allowed_handler(handler) # [公有] 注入自定义 405 处理器
// ============================================================================

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"

enum class DispatchResult {
    Matched,
    NotFound,
    MethodNotAllowed
};

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

// HttpRouter 负责将 HTTP 请求按精确匹配、405 判定、前缀兜底、404 回退的固定流程线性分发。
class HttpRouter {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
    using AllowedMethods = std::unordered_set<std::string>;

    HttpRouter();
    ~HttpRouter();

    DispatchResult dispatch(const HttpRequest& req, HttpResponse& resp) const; // 路由分发总入口。

    void add_route(const std::string& method, const std::string& path, Handler handler);
    void add_get_route(const std::string& path, Handler handler);
    void add_post_route(const std::string& path, Handler handler);
    void add_head_route(const std::string& path, Handler handler);
    void add_prefix_route(const std::string& prefix, Handler handler); // 注册前缀兜底路由。

    void set_not_found_handler(Handler handler);
    void set_method_not_allowed_handler(Handler handler);

private:
    struct PrefixRoute {
        std::string prefix;
        Handler handler;
    };

    const Handler* find_exact_handler(const HttpRequest& req) const;

    const AllowedMethods* find_allowed_methods(const std::string& path) const;
    void write_method_not_allowed_response(
        const HttpRequest& req,
        const AllowedMethods& allowedMethods,
        HttpResponse& resp) const;
    std::string format_allow_header(const AllowedMethods& allowedMethods) const; // 生成 Allow 头内容。

    const Handler* find_prefix_handler(const std::string& path) const;
    void write_not_found_response(const HttpRequest& req, HttpResponse& resp) const;

private:
    std::unordered_map<RouteKey, Handler, RouteKeyHash> exactRoutes_;           // 精确路由表，使用 method + path 锁定唯一处理器。
    std::unordered_map<std::string, AllowedMethods> allowedMethodsByPath_;      // 路径到允许方法集合的索引，用于严格区分 404 与 405。
    std::vector<PrefixRoute> prefixRoutes_;                                     // 前缀兜底路由表，保持注册顺序来表达匹配优先级。
    Handler notFoundHandler_;                                                   // 可选的 404 覆盖处理器，用于接管未命中响应内容。
    Handler methodNotAllowedHandler_;                                           // 可选的 405 覆盖处理器，用于接管方法不允许响应内容。
};