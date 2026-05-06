// ============================================================================ //
// Router.h
// HTTP 路由器对外契约，负责精确路由、方法约束与前缀兜底分发。
// ============================================================================ //

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"

/**
 * @brief 描述一次请求分发最终落入的路由分支。
 */
enum class DispatchResult {
    Matched,
    NotFound,
    MethodNotAllowed
};

/**
 * @brief 精确路由索引键，使用 method + path 唯一标识一个处理器。
 */
struct RouteKey {
    std::string method;
    std::string path;

    /**
     * @brief 判断两个精确路由键是否完全相同。
     * @param other 待比较的路由键契约。
     * @return bool method 与 path 都一致时返回 true。
     */
    bool operator==(const RouteKey& other) const;
};

/**
 * @brief 为精确路由键提供 unordered_map 所需的哈希策略。
 */
struct RouteKeyHash {
    /**
     * @brief 计算复合路由键的哈希值。
     * @param key 待计算哈希的精确路由键。
     * @return std::size_t 可用于哈希桶分配的哈希值。
     */
    std::size_t operator()(const RouteKey& key) const;
};

// Router 负责将 HTTP 请求按精确匹配、405 判定、前缀兜底、404 回退的固定流程线性分发。
class Router {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
    using AllowedMethods = std::unordered_set<std::string>;

    Router();
    ~Router();

    /**
     * @brief 执行单次请求分发，是 Router 唯一的业务流程入口。
     * @param req 外部传入的只读 HTTP 请求契约。
     * @param resp 由路由器或命中处理器填充的 HTTP 响应契约。
     * @return DispatchResult 本次分发最终落入的结果分支。
     */
    DispatchResult dispatch(const HttpRequest& req, HttpResponse& resp) const;

    /**
     * @brief 注册一个精确 method + path 路由。
     * @param method 路由允许的 HTTP 方法。
     * @param path 路由匹配的精确路径。
     * @param handler 命中后执行的处理器契约。
     */
    void add_route(const std::string& method, const std::string& path, Handler handler);

    /**
     * @brief 注册一个 GET 精确路由。
     * @param path 路由匹配的精确路径。
     * @param handler 命中后执行的处理器契约。
     */
    void add_get_route(const std::string& path, Handler handler);

    /**
     * @brief 注册一个 POST 精确路由。
     * @param path 路由匹配的精确路径。
     * @param handler 命中后执行的处理器契约。
     */
    void add_post_route(const std::string& path, Handler handler);

    /**
     * @brief 注册一个 HEAD 精确路由。
     * @param path 路由匹配的精确路径。
     * @param handler 命中后执行的处理器契约。
     */
    void add_head_route(const std::string& path, Handler handler);

    /**
     * @brief 注册一个按前缀匹配的兜底路由。
     * @param prefix 路径前缀契约。
     * @param handler 命中前缀后执行的处理器契约。
     */
    void add_prefix_route(const std::string& prefix, Handler handler);

    /**
     * @brief 注入自定义 404 处理器。
     * @param handler 当请求未命中任何路由时执行的处理器契约。
     */
    void set_not_found_handler(Handler handler);

    /**
     * @brief 注入自定义 405 处理器。
     * @param handler 当路径存在但方法不被允许时执行的处理器契约。
     */
    void set_method_not_allowed_handler(Handler handler);

private:
    struct PrefixRoute {
        std::string prefix;
        Handler handler;
    };

    /**
     * @brief 按请求的 method 与 path 查询精确路由处理器。
     * @param req 外部传入的只读 HTTP 请求契约。
     * @return const Handler* 命中时返回处理器地址，否则返回 nullptr。
     */
    const Handler* find_exact_handler(const HttpRequest& req) const;

    /**
     * @brief 执行一个已经选定的处理器。
     * @param handler 需要被调用的处理器契约。
     * @param req 外部传入的只读 HTTP 请求契约。
     * @param resp 由处理器填充的 HTTP 响应契约。
     */
    void execute_handler(const Handler& handler, const HttpRequest& req, HttpResponse& resp) const;

    /**
     * @brief 查询指定路径已注册的方法集合，用于区分 404 与 405。
     * @param path 当前请求路径。
     * @return const AllowedMethods* 路径存在时返回方法集合地址，否则返回 nullptr。
     */
    const AllowedMethods* find_allowed_methods(const std::string& path) const;

    /**
     * @brief 写入 405 响应契约。
     * @param req 外部传入的只读 HTTP 请求契约。
     * @param allowedMethods 当前路径允许的方法集合。
     * @param resp 待填充的 HTTP 响应契约。
     */
    void write_method_not_allowed_response(
        const HttpRequest& req,
        const AllowedMethods& allowedMethods,
        HttpResponse& resp) const;

    /**
     * @brief 使用路由器内建策略填充默认 405 响应。
     * @param allowedMethods 当前路径允许的方法集合。
     * @param resp 待填充的 HTTP 响应契约。
     */
    void fill_default_method_not_allowed_response(
        const AllowedMethods& allowedMethods,
        HttpResponse& resp) const;

    /**
     * @brief 用统一的纯文本模板填充标准响应。
     * @param statusCode HTTP 状态码。
     * @param reasonPhrase HTTP 原因短语。
     * @param body 需要写入的响应体文本。
     * @param resp 待填充的 HTTP 响应契约。
     */
    static void fill_plain_text_response(
        int statusCode,
        const std::string& reasonPhrase,
        const std::string& body,
        HttpResponse& resp);

    /**
     * @brief 将允许的方法集合格式化为 Allow 头值。
     * @param allowedMethods 当前路径允许的方法集合。
     * @return std::string 逗号分隔并排序后的 Allow 头内容。
     */
    std::string format_allow_header(const AllowedMethods& allowedMethods) const;

    /**
     * @brief 按注册顺序查找首个命中的前缀处理器。
     * @param path 当前请求路径。
     * @return const Handler* 命中时返回处理器地址，否则返回 nullptr。
     */
    const Handler* find_prefix_handler(const std::string& path) const;

    /**
     * @brief 判断一个字符串是否以前缀字符串开头。
     * @param text 待判断的完整文本。
     * @param prefix 需要匹配的前缀文本。
     * @return bool 以前缀开头时返回 true。
     */
    static bool starts_with(const std::string& text, const std::string& prefix);

    /**
     * @brief 写入 404 响应契约。
     * @param req 外部传入的只读 HTTP 请求契约。
     * @param resp 待填充的 HTTP 响应契约。
     */
    void write_not_found_response(const HttpRequest& req, HttpResponse& resp) const;

    /**
     * @brief 使用路由器内建策略填充默认 404 响应。
     * @param resp 待填充的 HTTP 响应契约。
     */
    void fill_default_not_found_response(HttpResponse& resp) const;

private:
    std::unordered_map<RouteKey, Handler, RouteKeyHash> exactRoutes_; // 精确路由表，使用 method + path 锁定唯一处理器。
    std::unordered_map<std::string, AllowedMethods> allowedMethodsByPath_; // 路径到允许方法集合的索引，用于严格区分 404 与 405。
    std::vector<PrefixRoute> prefixRoutes_; // 前缀兜底路由表，保持注册顺序来表达匹配优先级。
    Handler notFoundHandler_; // 可选的 404 覆盖处理器，用于接管未命中响应内容。
    Handler methodNotAllowedHandler_; // 可选的 405 覆盖处理器，用于接管方法不允许响应内容。
};
