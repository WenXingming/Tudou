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

// 一个简化版的 HttpServer，仅用于测试 HTTP 解析与业务回调
class HttpServer {
    using MessageCallback = std::function<void(const HttpRequest&, HttpResponse&)>;

private:
    std::string ip{ "127.0.0.1" };
    uint16_t port{ 8080 };
    std::unique_ptr<TcpServer> tcpServer;

    // 以连接 fd 作为 key 维护每个连接的 HttpContext
    std::unordered_map<int, std::unique_ptr<HttpContext>> httpContexts;

    MessageCallback messageCallback{ nullptr };

public:
    HttpServer(std::string ip, uint16_t port);
    ~HttpServer() = default;

    void set_message_callback(const MessageCallback& cb) { messageCallback = cb; }

    void start();

private:
    void connect_callback(const std::shared_ptr<TcpConnection>& conn);
    void message_callback(const std::shared_ptr<TcpConnection>& conn);
    void close_callback(const std::shared_ptr<TcpConnection>& conn);

    void handle_request(const std::shared_ptr<TcpConnection>& conn,
        HttpContext& ctx);

    std::string generate_404_page();

};

