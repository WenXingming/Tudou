// #pragma once

// #include <string>
// #include <memory>

// #include "../base/InetAddress.h"

// #include "../tudou/EventLoop.h"
// #include "../http/HttpServer.h"        // 测试用 HttpServer


// class TestHttpServer {
// public:
//     TestHttpServer(int port, const std::string& html_path);

//     // 启动 HTTP 服务器（阻塞在 EventLoop::loop）
//     void start();

// private:
//     void on_http_request(const HttpRequest& req, HttpResponse& resp);

// private:
//     int port_;
//     std::string html_path_;

//     EventLoop loop_;
//     InetAddress listen_addr_;
//     HttpServer http_server_;
// };
