/**
 * @file Router.h
 * @brief Minimal HTTP router: method + path -> handler 分发
 * @author wenxingming
 * @date 2026-01-31
 * @project: https://github.com/WenXingming/Tudou
 * @details Router 提供了一个简单的 HTTP 路由功能，可以根据请求的 HTTP 方法和 URL 路径将请求分发到不同的处理函数（handler）。
 * - 路由的 key 是 (Method, Path) 对，应对同一路径不同方法的处理需求，或同一方法不同路径的处理需求。
 * - 支持精确匹配和前缀匹配两种路由方式，并且可以自定义 404 和 405 响应。
 *
 * 用法概览：
 *   Router r;
 *   r.add_get_route("/health", handler);
 *   r.add_prefix_route("/static/", staticHandler);
 *   r.dispatch(req, resp); // 未命中会自动给 404/405
 */

#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"

enum class DispatchResult {
    // 不想把这些工具类嵌套放在 Router 类里，避免冗长的名字
    Matched,            // 命中并已执行 handler
    NotFound,           // 路径未注册
    MethodNotAllowed    // 路径存在但不支持该方法
};

struct RouteKey {
    // RouteKey：精确路由的 key。
    // 为什么要同时用 method + path？
    //  - 同一路径可以对应不同 HTTP 方法，例如：
    //    GET /file   -> 下载
    //    POST /file  -> 上传（/path 主要是做路由键，映射到相应的 handler 处理逻辑）
    std::string method;
    std::string path;
    bool operator==(const RouteKey& other) const {
        return method == other.method && path == other.path;
    }
};

struct RouteKeyHash {
    // 另一种方式是把 method + '\n' + path 拼接成一个字符串再 hash，其中 '\n' 是一个不太可能出现在 method/path 里的分隔符。
    std::size_t operator()(const RouteKey& key) const {
        // 组合 hash，避免构造临时字符串。
        const std::size_t h1 = std::hash<std::string>{}(key.method);
        const std::size_t h2 = std::hash<std::string>{}(key.path);
        // hash_combine (boost 风格)
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

class Router {
    // Handler：真正处理请求的“函数对象”。
    // 约定：handler 读取 req，并把 resp 填好（状态码/头/body）。和 HttpServer 的回调风格一致。
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

public:
    // 注册路由；请在服务启动前完成注册，当前实现未做并发防护。
    // 这里的“路由”可以理解为：
    //   (HTTP 方法, URL path) -> 处理函数 handler
    // 例如：
    //   ("GET", "/health") -> health_handler
    void add_route(const std::string& method, const std::string& path, Handler handler);
    void add_get_route(const std::string& path, Handler handler);
    void add_post_route(const std::string& path, Handler handler);
    void add_head_route(const std::string& path, Handler handler);

    // 按前缀兜底（常用于静态文件服务）：匹配 req.path 以 prefix 开头的请求。
    // 例如：prefix 为 "/static/"，那么 "/static/a.png"、"/static/app.js" 都会命中。
    // 注意：前缀路由不区分 method；它的目标是“兜底处理某一类 path”。
    void add_prefix_route(const std::string& prefix, Handler handler);

    // 最核心的分发函数：根据 req 的 method + path 找到对应的 handler 并执行，填充 resp。
    // 返回值表示分发结果（命中/未命中/方法不允许）。
    DispatchResult dispatch(const HttpRequest& req, HttpResponse& resp) const;

    // 自定义 404/405 响应（可选）
    void set_not_found_handler(Handler handler);
    void set_method_not_allowed_handler(Handler handler);

private:
    static bool starts_with(const std::string& text, const std::string& prefix); // 私有工具函数，判断 text 是否以 prefix 开头

    void fill_default_not_found(HttpResponse& resp) const;
    void fill_default_method_not_allowed(const std::string& path, HttpResponse& resp) const;
    std::string build_allow_header(const std::string& path) const;

private:
    // routes_：精确匹配路由表。
    // key = (method, path)，value = handler 相应的处理逻辑
    // 例如：routes_[{"GET", "/health"}] = health_handler
    std::unordered_map<RouteKey, Handler, RouteKeyHash> routes_;

    // allowed_methods_by_path_：辅助表，用来生成 405 Method Not Allowed。
    // 例如注册了：GET /health、POST /health
    // 那 allowedMethodsByPath_["/health"] = {"GET", "POST"}
    // 当收到 PUT /health 时：
    //  - 精确匹配找不到
    //  - 但是 path 存在（allowed_methods_by_path_ 有）
    //  - 应返回 405，并在 Allow 头里告诉客户端支持哪些方法
    std::unordered_map<std::string, std::unordered_set<std::string>> allowedMethodsByPath_;

    // prefix_routes_：前缀兜底路由。
    // 存储顺序 = 注册顺序，dispatch 时会按顺序依次尝试。pair: {prefix, handler}
    // 建议：把更“具体”的前缀先注册，把更“宽泛”的前缀（如 "/"）最后注册。
    std::vector<std::pair<std::string, Handler>> prefixRoutes_;

    // 自定义 404/405 的 handler 接口（可选）。
    // 不设置则走默认实现（纯文本 404/405）。
    Handler notFoundHandler_;
    Handler methodNotAllowedHandler_;
};
