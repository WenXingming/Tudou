/**
 * @file StaticFileHttpServer.cpp
 * @brief 静态文件 HTTP 服务器实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "StaticFileHttpServer.h"

#include <ctime>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

#include <sys/stat.h>

#include "tudou/http/HttpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/router/Router.h"
#include "spdlog/spdlog.h"

namespace {

// ---------------------------------------------------------------------------
// 自由辅助函数（不依赖 StaticFileHttpServer 成员）
// ---------------------------------------------------------------------------

std::string format_http_date_rfc1123(std::time_t t) {
    std::tm tmUtc{};
    if (::gmtime_r(&t, &tmUtc) == nullptr) {
        return "";
    }
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::put_time(&tmUtc, "%a, %d %b %Y %H:%M:%S GMT");
    return oss.str();
}

bool is_allowed_method(const std::string& method) {
    return method == "GET" || method == "HEAD";
}

bool get_file_meta(const std::string& path, std::time_t& mtime, long long& size) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    mtime = st.st_mtime;
    size = static_cast<long long>(st.st_size);
    return true;
}

std::string guess_content_type(const std::string& filepath) {
    auto pos = filepath.rfind('.');
    if (pos == std::string::npos) {
        return "application/octet-stream";
    }
    const std::string ext = filepath.substr(pos + 1);

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")                  return "text/css; charset=utf-8";
    if (ext == "js")                   return "text/javascript; charset=utf-8";
    if (ext == "txt")                  return "text/plain; charset=utf-8";
    if (ext == "json")                 return "application/json; charset=utf-8";
    if (ext == "webmanifest")          return "application/manifest+json";
    if (ext == "png")                  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")                  return "image/gif";
    if (ext == "svg")                  return "image/svg+xml";
    if (ext == "ico")                  return "image/x-icon";
    if (ext == "webp")                 return "image/webp";
    if (ext == "mp4")                  return "video/mp4";
    if (ext == "mp3")                  return "audio/mpeg";
    return "application/octet-stream";
}

// -- 响应打包 ----------------------------------------------------------------

void set_method_not_allowed(HttpResponse& resp) {
    resp.set_http_version("HTTP/1.1");
    resp.set_status(405, "Method Not Allowed");
    resp.add_header("Content-Type", "text/plain; charset=utf-8");
    resp.add_header("Allow", "GET, HEAD");
    resp.set_body("Method Not Allowed");
    resp.set_close_connection(true);
}

void set_not_found(HttpResponse& resp) {
    resp.set_http_version("HTTP/1.1");
    resp.set_status(404, "Not Found");
    resp.add_header("Content-Type", "text/plain; charset=utf-8");
    resp.set_body("Not Found");
    resp.set_close_connection(true);
}

void set_head_ok(HttpResponse& resp, const std::string& contentType, std::time_t mtime, long long size) {
    resp.set_http_version("HTTP/1.1");
    resp.set_status(200, "OK");
    resp.add_header("Content-Type", contentType);
    resp.add_header("Content-Length", std::to_string(size));
    const std::string lastModified = format_http_date_rfc1123(mtime);
    if (!lastModified.empty()) {
        resp.add_header("Last-Modified", lastModified);
    }
    resp.add_header("Connection", "Keep-Alive");
    resp.set_body("");
    resp.set_close_connection(false);
}

void set_file_ok(HttpResponse& resp, const std::string& contentType, const std::string& body) {
    resp.set_http_version("HTTP/1.1");
    resp.set_status(200, "OK");
    resp.add_header("Content-Type", contentType);
    resp.add_header("Content-Length", std::to_string(body.size()));
    resp.add_header("Connection", "Keep-Alive");
    resp.set_body(body);
    resp.set_close_connection(false);
}

} // namespace

// ===========================================================================
// StaticFileHttpServer
// ===========================================================================

StaticFileHttpServer::StaticFileHttpServer(StaticFileServerConfig cfg)
    : cfg_(std::move(cfg))
    , httpServer_(new HttpServer(cfg_.ip, cfg_.port, cfg_.threadNum))
    , router_(new Router()) {

    router_->add_prefix_route("/", [this](const HttpRequest& req, HttpResponse& resp) {
        on_http_request(req, resp);
        });

    httpServer_->set_http_callback([this](const HttpRequest& req, HttpResponse& resp) {
        router_->dispatch(req, resp);
        });
}

StaticFileHttpServer::~StaticFileHttpServer() = default;

void StaticFileHttpServer::start() {
    spdlog::info("StaticFileHttpServer listening on {}:{} with baseDir={}", cfg_.ip, cfg_.port, cfg_.baseDir);
    httpServer_->enable_ssl("certs/test-cert.pem", "certs/test-key.pem");
    httpServer_->start();
}

// ---------------------------------------------------------------------------
// 请求处理
// ---------------------------------------------------------------------------

void StaticFileHttpServer::on_http_request(const HttpRequest& req, HttpResponse& resp) {
    const std::string& method = req.get_method();

    if (!is_allowed_method(method)) {
        set_method_not_allowed(resp);
        return;
    }

    const std::string realPath = resolve_path(req.get_path());

    if (method == "HEAD") {
        std::time_t mtime = 0;
        long long size = 0;
        if (!get_file_meta(realPath, mtime, size)) {
            set_not_found(resp);
            resp.set_body("");
            return;
        }
        set_head_ok(resp, guess_content_type(realPath), mtime, size);
        return;
    }

    // GET
    package_file_response(realPath, resp);
}

void StaticFileHttpServer::package_file_response(const std::string& realPath, HttpResponse& resp) {
    // 先尝试缓存，再尝试磁盘
    std::time_t mtime = 0;
    long long fileSize = 0;
    if (!get_file_meta(realPath, mtime, fileSize)) {
        set_not_found(resp);
        return;
    }

    std::string content;

    // 查缓存
    {
        std::lock_guard<std::mutex> lock(fileCacheMutex_);
        auto it = fileCache_.find(realPath);
        if (it != fileCache_.end() && it->second.mtime == mtime && it->second.size == fileSize) {
            content = it->second.content;
        }
    }

    // 缓存未命中，从磁盘读取并更新缓存
    if (content.empty() && fileSize > 0) {
        std::ifstream ifs(realPath, std::ios::binary);
        if (!ifs) {
            set_not_found(resp);
            return;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        content = oss.str();

        std::lock_guard<std::mutex> lock(fileCacheMutex_);
        CacheEntry& entry = fileCache_[realPath];
        entry.content = content;
        entry.mtime = mtime;
        entry.size = fileSize;
    }

    set_file_ok(resp, guess_content_type(realPath), content);
}

// ---------------------------------------------------------------------------
// URL → 磁盘路径映射
// ---------------------------------------------------------------------------

std::string StaticFileHttpServer::resolve_path(const std::string& urlPath) const {
    std::string path = urlPath;
    if (path.empty()) {
        path = "/";
    }

    // 防止目录穿越
    if (path.find("..") != std::string::npos) {
        return cfg_.baseDir + "/__forbidden__";
    }

    if (path[0] != '/') {
        path.insert(path.begin(), '/');
    }

    std::string realPath = cfg_.baseDir + path;

    // 以 "/" 结尾或根路径 → 追加 index.html
    if (realPath.back() == '/') {
        return realPath + "index.html";
    }

    // 磁盘上是目录 → 追加 /index.html
    struct stat st;
    if (::stat(realPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return realPath + "/index.html";
    }

    if (path == "/") {
        return cfg_.baseDir + "/index.html";
    }

    return realPath;
}
