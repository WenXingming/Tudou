/**
 * @file StaticFileHttpServer.h
 * @brief 发送文件的 HTTP 服务器示例
 * @details 得益于 Tudou 框架的模块化设计，实现一个发送文件的 HTTP 服务器变得非常简单。只需持有 Tudou 提供的 HttpServer 类，并设置相应的回调函数即可完成文件发送功能
 * 静态文件 HTTP 服务器，用于测试 HttpServer：
 *   - 根据 URL 路径从指定根目录读取文件并返回
 *   - 例如：GET /hello-world.html -> <baseDir>/hello-world.html
 *   - 特殊规则："/" 映射为 "/index.html"（或者你可以根据需要修改）
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

#include "tudou/http/HttpServer.h"

class HttpServer;
class HttpRequest;
class HttpResponse;

class StaticFileHttpServer {
public:
    StaticFileHttpServer(const std::string& ip,
                         uint16_t port,
                         const std::string& baseDir,
                         int threadNum = 0);

    // 启动服务器（阻塞当前线程）
    void start();

private:
    void on_http_request(const HttpRequest& req, HttpResponse& resp); // 仅需设置消息处理回调即可
    std::string resolve_path(const std::string& urlPath) const;
    std::string guess_content_type(const std::string& filepath) const;
    bool get_file_content_cached(const std::string& realPath, std::string& content) const;

private:
    std::string ip_;
    uint16_t port_;
    std::string baseDir_;
    int threadNum_;

    std::unique_ptr<HttpServer> httpServer_;

    // 简单的文件内容缓存：避免每个请求都从磁盘读取同一个静态文件
    mutable std::mutex cacheMutex_;
    mutable std::unordered_map<std::string, std::string> fileCache_;
};
