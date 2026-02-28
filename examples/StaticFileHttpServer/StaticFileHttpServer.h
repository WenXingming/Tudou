/**
 * @file StaticFileHttpServer.h
 * @brief 静态文件 HTTP 服务器：根据 URL 路径从 baseDir 读取文件并返回
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "StaticFileServerConfig.h"

class HttpServer;
class HttpRequest;
class HttpResponse;
class Router;

class StaticFileHttpServer {
public:
    explicit StaticFileHttpServer(StaticFileServerConfig cfg);
    ~StaticFileHttpServer();

    void start();

private:
    void on_http_request(const HttpRequest& req, HttpResponse& resp);

    void package_file_response(const std::string& realPath, HttpResponse& resp);
    std::string resolve_path(const std::string& urlPath) const;

    StaticFileServerConfig cfg_;
    std::unique_ptr<HttpServer> httpServer_;
    std::unique_ptr<Router> router_;

    struct CacheEntry {
        std::string content;
        std::time_t mtime;
        long long   size;
    };
    mutable std::mutex fileCacheMutex_;
    mutable std::unordered_map<std::string, CacheEntry> fileCache_;
};
