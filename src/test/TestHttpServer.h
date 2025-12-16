#pragma once

#include <string>

class HttpServer;
class HttpRequest;
class HttpResponse;

// 简单静态文件 HTTP 服务器，用于测试 HttpServer：
//  - 根据 URL 路径从指定根目录读取文件并返回
//  - 例如：GET /hello-world.html -> <baseDir>/hello-world.html
//  - 特殊规则："/" 映射为 "/homepage.html"（或者你可以根据需要修改）
class TestHttpServer {
public:
    TestHttpServer(const std::string& ip,
                   uint16_t port,
                   const std::string& baseDir,
                   int threadNum = 0);

    // 启动服务器（阻塞当前线程）
    void start();

private:
    void on_http_request(const HttpRequest& req, HttpResponse& resp);
    std::string resolve_path(const std::string& urlPath) const;
    std::string guess_content_type(const std::string& filepath) const;

private:
    std::string ip_;
    uint16_t port_;
    std::string baseDir_;
    int threadNum_;

    HttpServer* httpServer_; // 延后定义，实际在 .cpp 中用具体类型
};
