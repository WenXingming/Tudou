/**
 * @file StaticFileHttpServer.h
 * @brief 发送文件的 HTTP 服务器示例
 * @details 得益于 Tudou 框架的模块化设计，实现一个发送文件的 HTTP 服务器变得非常简单。只需持有 Tudou 提供的 HttpServer 类，并设置相应的回调函数即可完成文件发送功能
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include "StaticFileHttpServer.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cerrno>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sys/stat.h>
#include "tudou/http/HttpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "spdlog/spdlog.h"

inline std::string format_http_date_rfc1123(std::time_t t) {
    // 将 time_t 转换为 RFC1123 格式的 HTTP 日期字符串
    std::tm tmUtc{};
    if (::gmtime_r(&t, &tmUtc) == nullptr) {
        return "";
    }

    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    // RFC1123 / HTTP-date, always GMT
    oss << std::put_time(&tmUtc, "%a, %d %b %Y %H:%M:%S GMT");
    return oss.str();
}

StaticFileHttpServer::StaticFileHttpServer(const std::string& ip, uint16_t port, const std::string& baseDir, int threadNum) :
    ip_(ip),
    port_(port),
    baseDir_(baseDir),
    threadNum_(threadNum),
    httpServer_(nullptr),
    router_(),
    fileCacheMutex_(),
    fileCache_() {

    // 创建 HttpServer 实例 和 Router 实例
    httpServer_.reset(new HttpServer(ip_, port_, threadNum_));
    router_.reset(new Router());

    // 静态文件服务：用 prefix 兜底所有 path，具体 404/405 逻辑仍在 on_http_request 内处理。
    router_->add_prefix_route("/", [this](const HttpRequest& req, HttpResponse& resp) {
        on_http_request(req, resp);
        });

    // 数据流向：Tcp --> HttpRequest --> Router --> handler --> HttpResponse --> Tcp
    // 因此只需设置 Router 作为 HttpServer 的回调即可（HttpRequest --> Router）
    httpServer_->set_http_callback(
        [this](const HttpRequest& req, HttpResponse& resp) {
            router_->dispatch(req, resp);
        });
}

void StaticFileHttpServer::start() {
    spdlog::info("StaticFileHttpServer listening on {}:{} with baseDir={}", ip_, port_, baseDir_);
    httpServer_->start();
}

void StaticFileHttpServer::on_http_request(const HttpRequest& req, HttpResponse& resp) {
    const std::string& method = req.get_method();
    const std::string& path = req.get_path();

    // 静态文件服务器仅支持 GET 和 HEAD 方法
    if (is_not_get_and_head(method)) {
        this->package_method_not_allowed_response(resp);
        return;
    }

    std::string realPath = resolve_path(path);

    // HEAD 方法特殊处理：只返回头部，不返回文件内容
    if (method == "HEAD") {
        std::time_t mtime = 0;
        long long size = 0;
        if (!get_file_meta(realPath, mtime, size)) {
            this->package_not_found_response(resp);
            resp.set_body(""); // HEAD 方法不返回 body 即 "Not Found"
            return;
        }

        std::string contentType = guess_content_type(realPath);
        this->package_metadata_response(resp, contentType, mtime, size);
        return;
    }

    // GET 方法：返回文件内容
    this->package_file_response(realPath, resp);
}

bool StaticFileHttpServer::is_not_get_and_head(const std::string& method) const {
    const bool isGet = (method == "GET");
    const bool isHead = (method == "HEAD");
    return !isGet && !isHead; // or: return !(isGet || isHead);
}

void StaticFileHttpServer::package_method_not_allowed_response(HttpResponse& resp) const {
    resp.set_http_version("HTTP/1.1");
    resp.set_status(405, "Method Not Allowed");
    resp.add_header("Content-Type", "text/plain; charset=utf-8");
    resp.add_header("Allow", "GET, HEAD");
    resp.set_body("Method Not Allowed");
    resp.set_close_connection(true);
}

void StaticFileHttpServer::package_not_found_response(HttpResponse& resp) const {
    resp.set_http_version("HTTP/1.1");
    resp.set_status(404, "Not Found");
    resp.add_header("Content-Type", "text/plain; charset=utf-8");
    resp.set_body("Not Found");
    resp.set_close_connection(true);
}

bool StaticFileHttpServer::get_file_meta(const std::string& path, std::time_t& mtime, long long& size) const {
    // 获取文件的元信息：修改时间和大小
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    mtime = st.st_mtime;
    size = static_cast<long long>(st.st_size);
    return true;
}

void StaticFileHttpServer::package_metadata_response(HttpResponse& resp, std::string contentType, std::time_t mtime, long long size) const {
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

void StaticFileHttpServer::package_file_response(const std::string& realPath, HttpResponse& resp) {
    std::string fileContent;
    // 尝试从缓存获取文件内容
    if (get_file_content_from_cached(realPath, fileContent)) {
        resp.set_http_version("HTTP/1.1");
        resp.set_status(200, "OK");
        resp.set_body(fileContent);
        resp.add_header("Content-Type", guess_content_type(realPath));
        resp.add_header("Content-Length", std::to_string(fileContent.size()));
        resp.add_header("Connection", "Keep-Alive");
        resp.set_close_connection(false);
        return;
    }
    // 缓存未命中或已过期，尝试从磁盘读取
    if (!get_file_content_from_disk(realPath, fileContent)) {
        this->package_not_found_response(resp);
        return;
    }
    resp.set_http_version("HTTP/1.1");
    resp.set_status(200, "OK");
    resp.set_body(fileContent);
    resp.add_header("Content-Type", guess_content_type(realPath));
    resp.add_header("Content-Length", std::to_string(fileContent.size()));
    resp.add_header("Connection", "Keep-Alive");
    resp.set_close_connection(false);
}

bool StaticFileHttpServer::get_file_content_from_cached(const std::string& realPath, std::string& content) const {
    // TODO: 同一次请求 cache miss 时会 stat 两次（from cached + from disk），可优化为只 stat 一次
    // 获取文件的元信息
    std::time_t mtime = 0;
    long long size = 0;
    if (!get_file_meta(realPath, mtime, size)) {
        return false;
    }
    // 先检查缓存: 缓存命中且未过期则直接返回
    {
        std::lock_guard<std::mutex> lock(fileCacheMutex_);
        auto findIt = fileCache_.find(realPath);
        if (findIt != fileCache_.end() && findIt->second.mtime == mtime && findIt->second.size == size) {
            const CacheEntry& entry = findIt->second;
            content = entry.content;
            return true;
        }
    }
    // 缓存未命中或已过期，返回 false（后续需要尝试从磁盘读取并更新缓存）
    return false;
}

bool StaticFileHttpServer::get_file_content_from_disk(const std::string& realPath, std::string& content) const {
    // 获取文件的元信息
    std::time_t mtime = 0;
    long long size = 0;
    if (!get_file_meta(realPath, mtime, size)) {
        return false;
    }
    // 尝试从磁盘读取
    std::ifstream ifs(realPath, std::ios::binary);
    if (!ifs) {
        return false;
    }
    // 读取文件内容
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string loaded = oss.str();
    // 更新缓存
    {
        std::lock_guard<std::mutex> lock(fileCacheMutex_);
        CacheEntry& entry = fileCache_[realPath];
        entry.content = std::move(loaded);
        entry.mtime = mtime;
        entry.size = size;
        content = entry.content;
    }
    return true;
}

std::string StaticFileHttpServer::resolve_path(const std::string& urlPath) const {
    // 简单路径解析：
    //  - 空或 "/" -> "/index.html"
    //  - 目录请求（以 "/" 结尾，或实际为目录） -> 追加 "/index.html"
    //  - 其他：直接使用 urlPath

    std::string path = urlPath;
    if (path.empty()) {
        path = "/";
    }

    // 简单防止目录穿越：包含 ".." 的路径一律映射为 404 对应的虚构文件
    if (path.find("..") != std::string::npos) {
        return baseDir_ + "/__forbidden__"; // 不存在的路径，触发 404
    }

    // 确保以 "/" 开头
    if (!path.empty() && path[0] != '/') {
        path.insert(path.begin(), '/');
    }

    // 将 URL 映射为磁盘路径
    std::string realPath = baseDir_ + path;

    // 规则 1：URL 以 "/" 结尾，视为目录，追加 index.html
    if (!realPath.empty() && realPath.back() == '/') {
        realPath += "index.html";
        return realPath;
    }

    // 规则 2：URL 不以 "/" 结尾，但磁盘上确实是目录，也追加 index.html
    struct stat st;
    if (::stat(realPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        if (!realPath.empty() && realPath.back() != '/') {
            realPath += '/';
        }
        realPath += "index.html";
    }

    // 规则 3：根路径单独处理（兼容老行为）
    if (path == "/") {
        return baseDir_ + "/index.html";
    }

    return realPath;
}

std::string StaticFileHttpServer::guess_content_type(const std::string& filepath) const {
    // 根据文件扩展名猜测 Content-Type
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
