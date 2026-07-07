// ============================================================================
// HttpServer.h
// HTTP/HTTPS 服务器门面，把传输层事件压平成“读取、解析、执行业务、发送响应”。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// HttpServer.h
// └── HttpServer
//     ├── HttpServer(ip, port, threadNum)        # [公有] 构造服务器并绑定底层 TcpServer 回调
//     │   └── bind_tcp_callbacks()               # [私有] 绑定连接/消息/关闭事件
//     │       ├── on_connect(conn)               # [私有] 创建并登记连接级状态
//     │       │   └── create_connection_state(conn) const # [私有] 创建 HttpContext 与可选 TLS 状态
//     │       ├── on_message(conn)               # [私有] 处理一次消息到达，并按需从 conn 读取数据
//     │       │   ├── find_connection_state(conn) # [私有] 查找连接级状态
//     │       │   ├── read_request_payload(conn, data, state, payload) # [私有] 归一化本次 HTTP 明文
//     │       │   ├── log_incomplete_request(conn) # [私有] 记录等待更多数据
//     │       │   ├── reject_bad_request(conn, state) # [私有] 返回 400 并重置上下文
//     │       │   │   ├── build_bad_request_response()   # [私有] 构建 400 响应
//     │       │   │   ├── send_http_response(conn, state, resp) # [私有] 发送响应
//     │       │   │   │   ├── finalize_http_response(resp) # [私有] 补齐协议头
//     │       │   │   │   ├── serialize_response(resp)   # [私有] 序列化响应
//     │       │   │   │   └── send_response(conn, state, data) # [私有] 发送明文或 TLS 密文
//     │       │   │   └── HttpContext::reset()    # [私有] 清空本连接当前解析状态
//     │       │   └── reply_complete_request(conn, state) # [私有] 路由分发并发送响应
//     │       │       ├── build_http_response(req)       # [私有] 交给内部 Router 填充响应
//     │       │       ├── send_http_response(conn, state, resp) # [私有] 发送响应
//     │       │       │   ├── finalize_http_response(resp) # [私有] 补齐协议头
//     │       │       │   ├── serialize_response(resp)   # [私有] 序列化响应
//     │       │       │   └── send_response(conn, state, data) # [私有] 发送明文或 TLS 密文
//     │       │       └── HttpContext::reset()    # [私有] 清空本连接当前解析状态
//     │       └── on_close(conn)                 # [私有] 清理连接级状态
//     │           └── remove_connection_state(conn) # [私有] 删除连接级状态
//     ├── HttpServer(copy)                       # [公有] 删除拷贝语义
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值
//     ├── ~HttpServer()                          # [公有] 默认析构
//     ├── start()                                # [公有] 启动底层 TCP 服务
//     ├── add_route(method, path, handler)       # [公有] 注册 method + path 精确路由
//     ├── add_get_route(path, handler)           # [公有] 注册 GET 精确路由
//     ├── add_post_route(path, handler)          # [公有] 注册 POST 精确路由
//     ├── add_head_route(path, handler)          # [公有] 注册 HEAD 精确路由
//     ├── add_prefix_route(prefix, handler)      # [公有] 注册前缀兜底路由
//     ├── set_not_found_handler(handler)         # [公有] 覆盖默认 404 响应
//     ├── set_method_not_allowed_handler(handler) # [公有] 覆盖默认 405 响应
//     ├── enable_ssl(certFile, keyFile)          # [公有] 启用 HTTPS 支持
//     ├── is_ssl_enabled() const                 # [公有] 判断 TLS 是否已启用
// ============================================================================

#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "tudou/tcp/TcpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/HttpContext.h"
#include "tudou/http/TlsConfig.h"
#include "tudou/http/TlsConnection.h"
#include "tudou/http/Router.h"

class HttpServer {
public:
    using Handler = Router::Handler;

    HttpServer(std::string ip, uint16_t port, int threadNum = 0);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer() = default;

    void start();
    void add_route(const std::string& method, const std::string& path, Handler handler);
    void add_get_route(const std::string& path, Handler handler);
    void add_post_route(const std::string& path, Handler handler);
    void add_head_route(const std::string& path, Handler handler);
    void add_prefix_route(const std::string& prefix, Handler handler);
    void set_not_found_handler(Handler handler);
    void set_method_not_allowed_handler(Handler handler);
    bool enable_ssl(const std::string& certFile, const std::string& keyFile); // 在 start 前启用 HTTPS。

    bool is_ssl_enabled() const;

private:
    struct ConnectionState {
        HttpContext httpContext;                                                                // 单连接 HTTP 解析状态。
        std::unique_ptr<TlsConnection> tlsConnection;                                           // HTTPS 连接独有的 TLS 状态。
    };

    void bind_tcp_callbacks();
    void on_connect(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn);
    std::shared_ptr<ConnectionState> create_connection_state(const TcpConnectionPtr& conn) const;
    void on_close(const TcpConnectionPtr& conn);
    bool read_request_payload(
        const TcpConnectionPtr& conn,
        const std::string& receivedData,
        ConnectionState& state,
        std::string& payload) const; // 读取并归一化本次 HTTP 明文。

    std::shared_ptr<ConnectionState> find_connection_state(const TcpConnectionPtr& conn);
    void log_incomplete_request(const TcpConnectionPtr& conn) const;
    void reject_bad_request(const TcpConnectionPtr& conn, ConnectionState& state);
    void reply_complete_request(const TcpConnectionPtr& conn, ConnectionState& state);
    HttpResponse build_http_response(const HttpRequest& req) const; // 调用内部路由器构建响应。

    HttpResponse build_bad_request_response() const;
    void finalize_http_response(HttpResponse& resp) const;
    std::string serialize_response(const HttpResponse& resp) const;
    void send_http_response(const TcpConnectionPtr& conn,
        const ConnectionState& state,
        HttpResponse resp);
    void send_response(const TcpConnectionPtr& conn,
        const ConnectionState& state,
        const std::string& response); // 发送明文或 TLS 密文。

    void remove_connection_state(const TcpConnectionPtr& conn);

private:
    std::string ip_;                                                                            // 服务监听 IP。
    uint16_t port_;                                                                             // 服务监听端口。
    std::unique_ptr<TcpServer> tcpServer_;                                                      // 底层 TCP 服务器门面。

    std::unordered_map<TcpConnection*, std::shared_ptr<ConnectionState>> connectionStates_;     // 每条连接持有独立解析/TLS 状态，查找后可安全脱锁使用。
    std::mutex contextsMutex_;                                                                  // 保护连接级状态映射。

    Router router_;                                                                             // HTTP 路由器，统一持有精确路由、前缀路由与默认 404/405 策略。

    std::unique_ptr<TlsConfig> tlsConfig_;                                                      // 全局 TLS 配置，持有证书与私钥。
};
