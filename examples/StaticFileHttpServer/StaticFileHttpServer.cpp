/**
 * @file StaticFileHttpServer.h
 * @brief 发送文件的 HTTP 服务器示例
 * @details 得益于 Tudou 框架的模块化设计，实现一个发送文件的 HTTP 服务器变得非常简单。只需持有 Tudou 提供的 HttpServer 类，并设置相应的回调函数即可完成文件发送功能
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include "StaticFileHttpServer.h"

#include <fstream>
#include <sstream>
#include <cerrno>
#include <sys/stat.h>

#include "tudou/http/HttpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "spdlog/spdlog.h"

namespace {
bool get_file_meta(const std::string& path, std::time_t& mtime, long long& size) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    mtime = st.st_mtime;
    size = static_cast<long long>(st.st_size);
    return true;
}
} // namespace

StaticFileHttpServer::StaticFileHttpServer(const std::string& ip,
                                           uint16_t port,
                                           const std::string& baseDir,
                                           int threadNum):
    ip_(ip),
    port_(port),
    baseDir_(baseDir),
    threadNum_(threadNum),
    httpServer_(nullptr) {

    httpServer_.reset(new HttpServer(ip_, port_, threadNum_));
    httpServer_->set_http_callback(
        [this](const HttpRequest& req, HttpResponse& resp) {
            on_http_request(req, resp);
        });
}

void StaticFileHttpServer::start() {
    spdlog::info("StaticFileHttpServer listening on {}:{} with baseDir={}", ip_, port_, baseDir_);
    httpServer_->start();
}

void StaticFileHttpServer::on_http_request(const HttpRequest& req, HttpResponse& resp) {
    const std::string& method = req.get_method();
    const std::string& path = req.get_path();

    spdlog::debug("StaticFileHttpServer: method={}, path={}", method, path);

    const bool isGet = (method == "GET");
    const bool isHead = (method == "HEAD");
    if (!isGet && !isHead) {
        resp.set_status(405, "Method Not Allowed");
        resp.set_body("Method Not Allowed");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.add_header("Allow", "GET, HEAD");
        resp.set_close_connection(true);
        return;
    }

    std::string realPath = resolve_path(path);

    // HEAD: only check existence + metadata, do not send body.
    if (isHead) {
        std::time_t mtime = 0;
        long long size = 0;
        if (!get_file_meta(realPath, mtime, size)) {
            spdlog::warn("StaticFileHttpServer: file not found: {}", realPath);
            resp.set_status(404, "Not Found");
            resp.set_body("Not Found");
            resp.add_header("Content-Type", "text/plain; charset=utf-8");
            resp.set_close_connection(true);
            return;
        }

        resp.set_status(200, "OK");
        resp.set_body("");
        resp.add_header("Content-Type", guess_content_type(realPath));
        // For HEAD, Content-Length should reflect the body size that would be sent for GET.
        resp.add_header("Content-Length", std::to_string(size));
        resp.set_close_connection(false);
        resp.add_header("Connection", "Keep-Alive");
        return;
    }

    std::string fileContent;
    if (!get_file_content_cached(realPath, fileContent)) {
        spdlog::warn("StaticFileHttpServer: file not found: {}", realPath);
        resp.set_status(404, "Not Found");
        resp.set_body("Not Found");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(true);
        return;
    }

    resp.set_status(200, "OK");
    resp.set_body(fileContent);
    resp.add_header("Content-Type", guess_content_type(realPath));
    // Content-Length 由 HttpServer 在内部自动添加
    // 为了与 StaticFileTcpServer 测试更可比，这里使用 Keep-Alive
    resp.set_close_connection(false);
    resp.add_header("Connection", "Keep-Alive");
}

std::string StaticFileHttpServer::resolve_path(const std::string& urlPath) const {
    // 简单路径解析：
    //  - 空或 "/" -> "/index.html"
    //  - 其他：直接使用 urlPath

    std::string path = urlPath;
    if (path.empty() || path == "/") {
        path = "/index.html";
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
    if (ext == "css") {
        return "text/css; charset=utf-8";
    }
    if (ext == "js") {
        // 对于 ES Module，必须是 JS MIME 类型，否则浏览器会因为 MIME 不匹配拒绝执行
        return "text/javascript; charset=utf-8";
    }
    if (ext == "txt") {
        return "text/plain; charset=utf-8";
    }
    if (ext == "json") {
        return "application/json; charset=utf-8";
    }
    if (ext == "webmanifest") {
        return "application/manifest+json";
    }
    if (ext == "png") {
        return "image/png";
    }
    if (ext == "jpg" || ext == "jpeg") {
        return "image/jpeg";
    }
    if (ext == "gif") {
        return "image/gif";
    }
    if (ext == "svg") {
        return "image/svg+xml";
    }
    if (ext == "ico") {
        return "image/x-icon";
    }
    if (ext == "webp") {
        return "image/webp";
    }
    if (ext == "mp4") {
        return "video/mp4";
    }
    if (ext == "mp3") {
        return "audio/mpeg";
    }
    return "application/octet-stream";
}

bool StaticFileHttpServer::get_file_content_cached(const std::string& realPath, std::string& content) const {
    std::time_t mtime = 0;
    long long size = 0;
    if (!get_file_meta(realPath, mtime, size)) {
        return false;
    }
    // 先检查缓存
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = fileCache_.find(realPath);
        if (it != fileCache_.end() && it->second.mtime == mtime && it->second.size == size) {
            content = it->second.content;
            return true;
        }
    }

    // 缓存不存在或已过期，尝试从磁盘读取
    std::ifstream ifs(realPath, std::ios::binary);
    if (!ifs) {
        return false;
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string loaded = oss.str();

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        CacheEntry& entry = fileCache_[realPath];
        entry.content = std::move(loaded);
        entry.mtime = mtime;
        entry.size = size;
        content = entry.content;
    }
    return true;
}
