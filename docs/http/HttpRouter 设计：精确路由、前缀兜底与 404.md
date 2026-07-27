# HttpRouter 设计：精确路由、前缀兜底与 404

`HttpRouter` 负责把解析完成的 `HttpRequest` 交给业务处理器。它只处理路由匹配和未命中响应，不读取 Socket、不解析字节流，也不管理连接生命周期。

# Situation — 情境

`HttpContext` 将 TCP 字节流解析为结构化请求后，服务器还需要根据 `method + path` 找到业务处理器。

如果把这些判断直接写在 `HttpServer` 中，随着登录、文件、聊天等接口增加，服务器主流程会混入大量业务分支。因此项目使用独立的 Router，把“请求交给谁”从网络收发流程中分离出来。

# Task — 任务

Router 只保留基本路由能力：

1. 支持 `method + path` 精确匹配；
2. 支持静态资源和动态路径使用的前缀兜底；
3. 精确和前缀路由都未命中时返回 404；
4. 让业务方通过注册处理器接入，不修改 Router 主流程。

项目没有在 Router 层自动区分 404 和 405。方法限制由具体业务处理器决定，这样不需要维护额外的允许方法索引，也不把未实现的 HTTP 错误语义扩展到路由核心。

# Action — 设计与实现

## 1. 固定分发流程

Router 的流程只有三个阶段：

```text
精确路由匹配
    ↓ 未命中
按注册顺序扫描前缀路由
    ↓ 仍未命中
返回 404
```

```cpp
void HttpRouter::dispatch(
    const HttpRequest& req, HttpResponse& resp) const {
    const Handler* exactHandler = find_exact_handler(req);
    if (exactHandler != nullptr) {
        (*exactHandler)(req, resp);
        return;
    }

    const Handler* prefixHandler = find_prefix_handler(req.get_path());
    if (prefixHandler != nullptr) {
        (*prefixHandler)(req, resp);
        return;
    }

    resp = HttpResponse{};
    resp.set_status(404, "Not Found");
    resp.set_header("Content-Type", "text/plain");
    resp.set_body("Not Found");
    resp.set_header("Connection", "close");
}
```

`dispatch()` 总会通过匹配的 Handler 或默认 404 填充 `HttpResponse`，调用方不需要额外判断返回状态，因此接口直接返回 `void`。

业务如果需要自定义兜底响应，可以注册 `"/"` 前缀路由，不需要 Router 再维护一套专用的 404 Handler 状态。

精确路由必须先于前缀路由。这样明确注册的 API 不会被 `/` 或 `/static` 之类的兜底处理器提前接管。

## 2. 精确路由：`Method + Path` 哈希查找

精确路由使用联合键：

```cpp
struct RouteKey {
    std::string method;
    std::string path;

    bool operator==(const RouteKey& other) const {
        return method == other.method && path == other.path;
    }
};
```

路由表为：

```cpp
std::unordered_map<RouteKey, Handler, RouteKeyHash> exactRoutes_;
```

因此 `GET /login` 和 `POST /login` 是两个独立入口，平均查找复杂度为 `O(1)`。

注册时只需要维护这一张表：

```cpp
void HttpRouter::add_route(
    const std::string& method,
    const std::string& path,
    Handler handler) {
    exactRoutes_[RouteKey{method, path}] = std::move(handler);
}
```

没有第二张 `Path -> AllowedMethods` 表，也就没有两张表同步的问题。

## 3. 前缀路由：线性兜底

前缀路由适合静态文件或统一入口：

```cpp
router.add_prefix_route("/static/", staticFileHandler);
router.add_prefix_route("/", defaultHandler);
```

它使用 `vector` 保存，注册顺序就是匹配优先级：

```cpp
void HttpRouter::add_prefix_route(
    const std::string& prefix,
    Handler handler) {
    prefixRoutes_.push_back(PrefixRoute{prefix, std::move(handler)});
}
```

查找属于兜底分支，使用简单的线性扫描即可。当前规则是“首个匹配的前缀优先”，不是最长前缀优先；如果多个前缀可能重叠，应在注册顺序上明确意图。

## 4. 方法限制由业务处理器负责

Router 只判断路由是否命中，不自动判断请求方法是否适合前缀处理器。使用前缀路由的业务代码需要自行检查方法：

```cpp
void StaticFileServer::on_request(
    const HttpRequest& req,
    HttpResponse& resp) {
    if (req.get_method() != "GET" && req.get_method() != "HEAD") {
        resp.set_status(404, "Not Found");
        resp.set_body("Not Found");
        return;
    }

    // 处理静态文件请求。
}
```

这是一项有意的简化：Router 不维护业务方法索引，业务模块自行决定不支持的方法如何响应。

# Result — 结果

- `HttpServer` 只负责接收请求、调用 Router 和发送响应；
- Router 的核心流程从四个分支收敛为三个阶段；
- 删除 `allowedMethodsByPath_` 和 405 专用处理逻辑；
- 路由注册只维护一张精确路由表；
- 前缀路由仍可支持静态资源和统一兜底；
- 业务方法限制在业务处理器内部表达，避免 Router 提前承担更多 HTTP 策略。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 职责分离 | `HttpServer` 负责网络和请求流程，`HttpRouter` 负责路由，Handler 负责业务。 |
| 精确匹配 | 使用 `Method + Path` 联合键，通过 `unordered_map` 快速定位处理器。 |
| 前缀兜底 | 使用 `vector` 保存前缀，注册顺序决定优先级。 |
| 错误策略 | 精确和前缀都未命中时返回 404；方法限制由业务 Handler 决定。 |
| 单一数据源 | 精确路由只维护一张表，不需要同步额外索引。 |

# 面试核心问答

## Q1：为什么设计独立 Router？

HTTP 解析和业务分发是两种不同职责。独立 Router 可以让 `HttpServer` 保持“读取、解析、路由、发送”的主流程，也方便业务模块集中注册处理器。

## Q2：为什么使用 `Method + Path` 作为联合键？

同一路径可以对应多个方法，例如 `GET /login` 和 `POST /login`。联合键可以直接定位唯一处理器，平均查找复杂度为 `O(1)`。

## Q3：为什么不在 Router 中维护 405 索引？

项目优先保证核心请求链路简单。Router 只负责判断路由是否命中；前缀处理器或业务 Handler 自行决定方法限制和错误响应，因此不需要维护第二张允许方法表。

## Q4：为什么前缀路由使用 `vector`？

前缀匹配不是完整字符串查找，而且当前注册顺序就是优先级。`vector` 能直接保存这个顺序，前缀路由只在精确匹配失败后执行，线性扫描成本可接受。

相关主题：[HttpServer 设计](<HttpServer 设计：协议编排、连接状态与生命周期安全.md>)、[HttpConnection 设计](<HttpConnection 设计：粘包、HTTP Pipeline 与协议字节转换.md>)。
