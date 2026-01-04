/**
 * @file Router.h
 * @brief Minimal HTTP router: method + path -> handler 分发
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

class Router {
public:
    // Handler：真正处理请求的“函数对象”。
    // 约定：handler 读取 req，并把 resp 填好（状态码/头/body）。
    // 为什么不返回 HttpResponse？
    //  - 你现在的 HttpServer 回调也是 (req, resp) 这种风格，Router 直接复用。
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

    enum class DispatchResult {
        Matched,            // 命中并已执行 handler
        NotFound,           // 路径未注册
        MethodNotAllowed    // 路径存在但不支持该方法
    };

    // 注册路由；请在服务启动前完成注册，当前实现未做并发防护。
    // 这里的“路由”可以理解为：
    //   (HTTP 方法, URL path) -> 处理函数 handler
    // 例如：
    //   ("GET", "/health") -> health_handler
    void add_route(const std::string& method, const std::string& path, Handler handler);

    // 显式命名：add_xxx_route 表示“注册某方法的路由”。
    // 这样读起来更像一句话：add GET route / add POST route。
    void add_get_route(const std::string& path, Handler handler);
    void add_post_route(const std::string& path, Handler handler);
    void add_head_route(const std::string& path, Handler handler);

    // 按前缀兜底（常用于静态文件）：匹配 req.path 以 prefix 开头的请求。
    // 例如：prefix 为 "/static/"，那么 "/static/a.png"、"/static/app.js" 都会命中。
    // 注意：前缀路由不区分 method；它的目标是“兜底处理某一类 path”。
    void add_prefix_route(const std::string& prefix, Handler handler);

    // 可选：自定义 404/405 响应
    void set_not_found_handler(Handler handler);
    void set_method_not_allowed_handler(Handler handler);

    // 分发请求，返回结果类别；若命中则已填充 resp
    DispatchResult dispatch(const HttpRequest& req, HttpResponse& resp) const;

private:
    // RouteKey：精确路由的 key。
    // 为什么要同时用 method+path？
    //  - 同一路径可以对应不同 HTTP 方法，例如：
    //    GET /file   -> 下载
    //    POST /file  -> 上传
    struct RouteKey {
        std::string method;
        std::string path;
        bool operator==(const RouteKey& other) const {
            return method == other.method && path == other.path;
        }
    };

    struct RouteKeyHash {
        std::size_t operator()(const RouteKey& key) const {
            // 用一个分隔符把 method 和 path 拼起来做哈希。
            // '\n' 只是一个不太会出现在 method/path 中的分隔符。
            return std::hash<std::string>()(key.method + "\n" + key.path);
        }
    };

    static bool starts_with(const std::string& text, const std::string& prefix);

    void fill_default_not_found(HttpResponse& resp) const;
    void fill_default_method_not_allowed(const std::string& path, HttpResponse& resp) const;
    std::string build_allow_header(const std::string& path) const;

    // routes_：精确匹配路由表。
    // key = (method, path)，value = handler
    // 例如：routes_[{"GET", "/health"}] = health_handler
    std::unordered_map<RouteKey, Handler, RouteKeyHash> routes_;

    // methods_by_path_：辅助表，用来生成 405 Method Not Allowed。
    // 例如注册了：GET /health、POST /health
    // 那 methods_by_path_["/health"] = {"GET", "POST"}
    // 当收到 PUT /health 时：
    //  - 精确匹配找不到
    //  - 但是 path 存在（methods_by_path_ 有）
    //  - 应返回 405，并在 Allow 头里告诉客户端支持哪些方法
    std::unordered_map<std::string, std::unordered_set<std::string>> methods_by_path_;

    // prefix_routes_：前缀兜底路由。
    // 存储顺序 = 注册顺序，dispatch 时会按顺序依次尝试。
    // 建议：把更“具体”的前缀先注册，把更“宽泛”的前缀（如 "/"）最后注册。
    std::vector<std::pair<std::string, Handler>> prefix_routes_;

    // 自定义 404/405 的 handler（可选）。
    // 不设置则走默认实现（纯文本 404/405）。
    Handler not_found_handler_;
    Handler method_not_allowed_handler_;
};
