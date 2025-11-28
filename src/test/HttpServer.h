#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "../base/InetAddress.h"


#include "../tudou/TcpServer.h"
#include "../tudou/TcpConnection.h"
#include "../tudou/EventLoop.h"

#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"
#include "../http/HttpContext.h"

namespace tudou {

// 一个简化版的 HttpServer，仅用于测试 HTTP 解析与业务回调
class HttpServer {
public:
    using HttpCallback = std::function<void(const HttpRequest&, HttpResponse&)>;

    HttpServer(EventLoop* loop, const InetAddress& listen_addr);

    void set_http_callback(const HttpCallback& cb) { httpCallback = cb; }

    void start();

private:
    void on_connection(const std::shared_ptr<TcpConnection>& conn);
    void on_message(const std::shared_ptr<TcpConnection>& conn);

    void handle_request(const std::shared_ptr<TcpConnection>& conn,
        HttpContext& ctx);

private:
    EventLoop* loop{ nullptr };
    TcpServer tcpServer;

    // 以连接 fd 作为 key 维护每个连接的 HttpContext
    std::unordered_map<int, std::unique_ptr<HttpContext>> contexts;

    HttpCallback httpCallback;
};

} // namespace tudou
