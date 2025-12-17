#include "StaticFileHttpServer.h"

#include <fstream>
#include <sstream>

#include "../http/HttpServer.h"
#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"
#include "spdlog/spdlog.h"

StaticFileHttpServer::StaticFileHttpServer(const std::string& ip,
                                           uint16_t port,
                                           const std::string& baseDir,
                                           int threadNum)
    : ip_(ip),
      port_(port),
      baseDir_(baseDir),
      threadNum_(threadNum),
      httpServer_(nullptr) {
}

void StaticFileHttpServer::start() {
    // 在这里创建 HttpServer 并设置回调，再启动
    HttpServer server(ip_, port_, threadNum_);
    httpServer_ = &server;

    server.set_http_callback(
        [this](const HttpRequest& req, HttpResponse& resp) {
            on_http_request(req, resp);
        });

    spdlog::info("StaticFileHttpServer listening on {}:{} with baseDir={}", ip_, port_, baseDir_);
    server.start();
}

void StaticFileHttpServer::on_http_request(const HttpRequest& req, HttpResponse& resp) {
    const std::string& method = req.get_method();
    const std::string& path = req.get_path();

    spdlog::debug("StaticFileHttpServer: method={}, path={}", method, path);

    if (method != "GET") {
        resp.set_status(405, "Method Not Allowed");
        resp.set_body("Method Not Allowed");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(true);
        return;
    }

    std::string realPath = resolve_path(path);

    std::ifstream ifs(realPath, std::ios::binary);
    if (!ifs) {
        spdlog::warn("StaticFileHttpServer: file not found: {}", realPath);
        resp.set_status(404, "Not Found");
        resp.set_body("Not Found");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(true);
        return;
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();

    resp.set_status(200, "OK");
    resp.set_body(oss.str());
    resp.add_header("Content-Type", guess_content_type(realPath));
    // Content-Length 由 HttpServer 在内部自动添加
    resp.set_close_connection(true); // 测试场景使用短连接
}

std::string StaticFileHttpServer::resolve_path(const std::string& urlPath) const {
    // 简单路径解析：
    //  - 空或 "/" -> "/homepage.html"
    //  - 其他：直接使用 urlPath

    std::string path = urlPath;
    if (path.empty() || path == "/") {
        path = "/homepage.html";
    }

    // 简单防止目录穿越：包含 ".." 的路径一律映射为 404 对应的虚构文件
    if (path.find("..") != std::string::npos) {
        return baseDir_ + "/__forbidden__"; // 不存在的路径，触发 404
    }

    // 确保以 "/" 开头
    if (!path.empty() && path[0] != '/') {
        path.insert(path.begin(), '/');
    }

    return baseDir_ + path;
}

std::string StaticFileHttpServer::guess_content_type(const std::string& filepath) const {
    auto pos = filepath.rfind('.');
    if (pos == std::string::npos) {
        return "application/octet-stream";
    }

    std::string ext = filepath.substr(pos + 1);
    if (ext == "html" || ext == "htm") {
        return "text/html; charset=utf-8";
    }
    if (ext == "txt") {
        return "text/plain; charset=utf-8";
    }
    if (ext == "json") {
        return "application/json; charset=utf-8";
    }

    return "application/octet-stream";
}
