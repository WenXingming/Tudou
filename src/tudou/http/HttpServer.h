// ============================================================================
// HTTP/HTTPS 服务门面：串联 TCP 回调、TLS 明文转换、HTTP 解析、路由与响应发送。
// 每条连接独立保存解析和 TLS 状态；不负责底层 fd、事件循环或业务处理。
// ============================================================================

#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "tudou/tcp/TcpServer.h"
#include "tudou/http/HttpConnection.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/TlsConfig.h"
#include "tudou/http/HttpRouter.h"

class HttpServer {
public:
    using Handler = HttpRouter::Handler;

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
    bool enable_ssl(const std::string& certFile, const std::string& keyFile); // 在 start 前启用 HTTPS。

private:
    void on_connect(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn);
    void on_close(const TcpConnectionPtr& conn);

    bool send_http_response(const TcpConnectionPtr& conn, HttpConnection& httpConnection, HttpResponse resp); // 返回是否已发起关闭（Connection: close 或编码失败）。

private:
    std::string ip_;                       // 服务监听 IP。
    uint16_t port_;                        // 服务监听端口。
    TcpServer tcpServer_;                  // 底层 TCP 服务器门面。

    std::unordered_map<TcpConnection*, std::shared_ptr<HttpConnection>> httpConnections_; // 每条 TcpConnection 对应一份 HTTP/TLS 状态，查找后可安全脱锁使用。
    std::mutex contextsMutex_;                                                            // 保护连接级状态映射。

    HttpRouter router_; // HTTP 路由器，统一持有精确路由、前缀路由与默认 404 策略。

    std::unique_ptr<TlsConfig> tlsConfig_; // 全局 TLS 配置，持有证书与私钥。
};
