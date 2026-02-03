/**
 * @file FileLinkServer.h
 * @brief HTTP 服务器实现
 * @details 负责 HTTP 路由与协议细节，调用 FileLinkService 完成具体业务逻辑。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include "FileLinkServer.h"

#include <fstream>
#include <sstream>
#include <string>
#include <time.h>

#include "spdlog/spdlog.h"

#include "metastore/InMemoryFileMetaStore.h"
#include "metastore/MysqlFileMetaStore.h"
#include "metacache/NoopFileMetaCache.h"
#include "metacache/RedisFileMetaCache.h"

#include "utils/HttpUtil.h"
#include "utils/Uuid.h"

#include <algorithm>
#include <cctype>

namespace {

std::string get_header_or_empty(const HttpRequest& req, const std::string& key) {
    try {
        return req.get_header(key);
    }
    catch (...) {
        return std::string();
    }
}

void set_keep_alive(HttpResponse& resp, bool keepAlive) {
    resp.set_close_connection(!keepAlive);
    resp.add_header("Connection", keepAlive ? "Keep-Alive" : "close");
}

void respond_text(HttpResponse& resp,
    int status,
    const char* reason,
    const std::string& body,
    bool keepAlive,
    const char* contentType) {
    resp.set_status(status, reason);
    resp.set_body(body);
    resp.add_header("Content-Type", contentType);
    set_keep_alive(resp, keepAlive);
}

void respond_plain(HttpResponse& resp,
    int status,
    const char* reason,
    const std::string& body,
    bool keepAlive) {
    respond_text(resp, status, reason, body, keepAlive, "text/plain; charset=utf-8");
}

void respond_json(HttpResponse& resp,
    int status,
    const char* reason,
    const std::string& json,
    bool keepAlive) {
    respond_text(resp, status, reason, json, keepAlive, "application/json; charset=utf-8");
}

static bool extract_json_string_field(const std::string& body,
    const std::string& key,
    std::string& outValue) {
    outValue.clear();
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = body.find(pat);
    if (pos == std::string::npos) {
        return false;
    }
    pos = body.find(':', pos + pat.size());
    if (pos == std::string::npos) {
        return false;
    }
    // Skip whitespace
    while (pos < body.size() && (body[pos] == ':' || body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\r' || body[pos] == '\n')) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') {
        return false;
    }
    ++pos;
    std::string out;
    out.reserve(256);
    while (pos < body.size()) {
        const char c = body[pos++];
        if (c == '"') {
            outValue = out;
            return true;
        }
        if (c == '\\' && pos < body.size()) {
            const char esc = body[pos++];
            switch (esc) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            default: out.push_back(esc); break;
            }
            continue;
        }
        out.push_back(c);
    }
    return false;
}

} // namespace

namespace {

std::shared_ptr<IFileMetaStore> create_meta_store_from_cfg(const FileLinkServerConfig& cfg) {
#if FILELINK_WITH_MYSQLCPPCONN
    if (cfg.mysqlEnabled) {
        return std::make_shared<MysqlFileMetaStore>(
            cfg.mysqlHost,
            cfg.mysqlPort,
            cfg.mysqlUser,
            cfg.mysqlPassword,
            cfg.mysqlDatabase);
    }
    return std::make_shared<InMemoryFileMetaStore>();
#else
    if (cfg.mysqlEnabled) {
        spdlog::warn("MySQL enabled in config but FileLinkServer was built without mysqlcppconn; falling back to InMemoryFileMetaStore.");
    }
    return std::make_shared<InMemoryFileMetaStore>();
#endif
}

std::shared_ptr<IFileMetaCache> create_meta_cache_from_cfg(const FileLinkServerConfig& cfg) {
#if FILELINK_WITH_HIREDIS
    if (cfg.redisEnabled) {
        return std::make_shared<RedisFileMetaCache>(cfg.redisHost, cfg.redisPort);
    }
    return std::make_shared<NoopFileMetaCache>();
#else
    if (cfg.redisEnabled) {
        spdlog::warn("Redis enabled in config but FileLinkServer was built without hiredis; falling back to NoopFileMetaCache.");
    }
    return std::make_shared<NoopFileMetaCache>();
#endif
}

} // namespace

FileLinkServer::FileLinkServer(FileLinkServerConfig cfg)
    : cfg_(std::move(cfg)),
    auth_(filelink::AuthConfig{ cfg_.authEnabled, cfg_.authUser, cfg_.authPassword, cfg_.authTokenTtlSeconds }),
    service_(nullptr),
    httpServer_(nullptr) {

    init();
}

FileLinkServer::~FileLinkServer() {
}

void FileLinkServer::start() {
    spdlog::info("FileLinkServer listening on {}:{} storageRoot={} threadNum={}",
        cfg_.ip, cfg_.port, cfg_.storageRoot, cfg_.threadNum);
    httpServer_->start();
}

void FileLinkServer::on_http_request(const HttpRequest& req, HttpResponse& resp) {
    spdlog::debug("FileLinkServer: method={}, path={}", req.get_method(), req.get_path());
    (void)router_.dispatch(req, resp);
}

void FileLinkServer::init() {
    httpServer_.reset(new HttpServer(cfg_.ip, cfg_.port, cfg_.threadNum));

    auto metaStore = create_meta_store_from_cfg(cfg_);
    auto metaCache = create_meta_cache_from_cfg(cfg_);

    FileSystemStorage storage(cfg_.storageRoot);
    service_.reset(new FileLinkService(std::move(storage), std::move(metaStore), std::move(metaCache)));

    // 路由注册（启动前完成注册；当前 Router 未做并发防护）
    // （已移除 /health 路由）

    router_.add_post_route("/login", [this](const HttpRequest& req, HttpResponse& resp) {
        handle_login(req, resp);
        });

    router_.add_post_route("/upload", [this](const HttpRequest& req, HttpResponse& resp) {
        handle_upload(req, resp);
        });

    // 动态路由：/file/{id}
    // 这里用 prefix 兜底，具体 fileId 解析仍由 handle_download 完成。
    router_.add_prefix_route("/file/", [this](const HttpRequest& req, HttpResponse& resp) {
        handle_download(req, resp);
        });

    // 静态文件服务：用前缀路由统一处理（放在最后，作为兜底）。
    router_.add_prefix_route("/", [this](const HttpRequest& req, HttpResponse& resp) {
        handle_static(req, resp);
        });

    httpServer_->set_http_callback(
        [this](const HttpRequest& req, HttpResponse& resp) {
            on_http_request(req, resp);
        });
}

void FileLinkServer::handle_static(const HttpRequest& req, HttpResponse& resp) {
    const std::string& method = req.get_method();
    if (method != "GET" && method != "HEAD") {
        respond_plain(resp, 405, "Method Not Allowed", "Method Not Allowed", false);
        resp.add_header("Allow", "GET, HEAD");
        return;
    }

    if (cfg_.webRoot.empty()) {
        respond_plain(resp, 404, "Not Found", "Not Found", true);
        return;
    }

    std::string realPath;
    if (!resolve_static_real_path(req.get_path(), realPath)) {
        respond_plain(resp, 404, "Not Found", "Not Found", true);
        return;
    }

    std::string body;
    if (!read_file_all(realPath, body)) {
        respond_plain(resp, 404, "Not Found", "Not Found", true);
        return;
    }
    if (method == "HEAD") {
        body.clear();
    }

    resp.set_status(200, "OK");
    resp.set_body(body);
    resp.add_header("Content-Type", filelink::guess_content_type_by_name(realPath));
    set_keep_alive(resp, true);
}

bool FileLinkServer::resolve_static_real_path(const std::string& requestPath, std::string& outRealPath) const {
    std::string urlPath = requestPath;
    if (urlPath.empty()) {
        urlPath = "/";
    }

    // 简单防止目录穿越
    if (urlPath.find("..") != std::string::npos) {
        return false;
    }

    const std::string index = cfg_.indexFile.empty() ? "index.html" : cfg_.indexFile;

    // "/" -> "/index.html"（或 cfg_.indexFile）
    if (urlPath == "/") {
        urlPath = std::string("/") + index;
    }

    // 目录请求（以 / 结尾）-> 追加 indexFile
    if (!urlPath.empty() && urlPath.back() == '/') {
        urlPath += index;
    }

    std::string realPath = cfg_.webRoot;
    if (!realPath.empty() && realPath.back() != '/') {
        realPath.push_back('/');
    }

    // urlPath 以 '/' 开头，拼接时去掉前导 '/'
    if (!urlPath.empty() && urlPath.front() == '/') {
        realPath += urlPath.substr(1);
    }
    else {
        realPath += urlPath;
    }

    outRealPath = std::move(realPath);
    return true;
}

bool FileLinkServer::read_file_all(const std::string& realPath, std::string& outBody) const {
    std::ifstream ifs(realPath, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    outBody = oss.str();
    return true;
}

bool FileLinkServer::require_auth(const HttpRequest& req, HttpResponse& resp) {
    if (!auth_.enabled()) {
        return true;
    }
    const std::string token = get_header_or_empty(req, "X-Auth-Token");
    if (!auth_.validate_token(token)) {
        respond_plain(resp, 401, "Unauthorized", "unauthorized", false);
        resp.add_header("WWW-Authenticate", "FileLinkServer");
        return false;
    }
    return true;
}

void FileLinkServer::handle_login(const HttpRequest& req, HttpResponse& resp) {
    if (!auth_.enabled()) {
        respond_plain(resp, 404, "Not Found", "Not Found", false);
        return;
    }

    // Request body: {"user":"...","password":"..."}
    std::string user;
    std::string password;
    if (!extract_json_string_field(req.get_body(), "user", user) ||
        !extract_json_string_field(req.get_body(), "password", password)) {
        respond_plain(resp, 400, "Bad Request", "missing user/password", false);
        return;
    }

    if (!auth_.check_credentials(user, password)) {
        respond_plain(resp, 401, "Unauthorized", "invalid credentials", false);
        return;
    }

    const std::string token = auth_.issue_token();
    const int ttl = cfg_.authTokenTtlSeconds > 0 ? cfg_.authTokenTtlSeconds : 3600;
    std::string json = std::string("{\"token\":\"") + filelink::json_escape_minimal(token) +
        "\",\"expiresIn\":" + std::to_string(ttl) + "}";

    respond_json(resp, 200, "OK", json, true);
    resp.add_header("Cache-Control", "no-store");
}

void FileLinkServer::handle_upload(const HttpRequest& req, HttpResponse& resp) {
    if (!require_auth(req, resp)) {
        return;
    }
    // 约定（为了最小可用，这里刻意不做 multipart/form-data）：
    //  - Body: 文件原始二进制内容
    //  - Header: X-File-Name: 原始文件名（可选）
    //  - Header: Content-Type: 可选
    // 注：multipart/form-data 可后续扩展，这里先做最小可用版本

    // 后端限制：最大 5GB（前端也会限制，但后端必须兜底）。
    static constexpr size_t kMaxUploadBytes = 5ULL * 1024 * 1024 * 1024;

    // 小文件：仍然允许走“内存 body”路径；大文件：HTTP 层会把 body 流式写到临时文件，并通过 header 传入路径。
    const std::string tempUploadPath = get_header_or_empty(req, "X-Temp-Upload-Path");
    int64_t tempUploadSize = -1;
    const std::string tempUploadSizeHeader = get_header_or_empty(req, "X-Temp-Upload-Size");
    if (!tempUploadSizeHeader.empty()) {
        try {
            tempUploadSize = static_cast<int64_t>(std::stoll(tempUploadSizeHeader));
        }
        catch (...) {
            tempUploadSize = -1;
        }
    }

    const std::string& body = req.get_body();
    if (tempUploadPath.empty()) {
        if (body.empty()) {
            respond_plain(resp, 400, "Bad Request", "empty body", false);
            return;
        }
        if (body.size() > kMaxUploadBytes) {
            respond_plain(resp, 413, "Payload Too Large", "payload too large (max 5GB)", false);
            return;
        }
    }
    else {
        if (tempUploadSize > static_cast<int64_t>(kMaxUploadBytes)) {
            respond_plain(resp, 413, "Payload Too Large", "payload too large (max 5GB)", false);
            return;
        }
    }

    const std::string fileNameHeader = get_header_or_empty(req, "X-File-Name");
    const std::string fileName = fileNameHeader.empty() ? std::string() : filelink::url_decode(fileNameHeader);
    const std::string contentType = get_header_or_empty(req, "Content-Type");

    // 业务编排交给 service：
    // - 生成 fileId
    // - 落盘
    // - 写入元数据 + cache
    UploadResult r;
    if (!tempUploadPath.empty()) {
        r = service_->upload_from_path(fileName, contentType, tempUploadPath, tempUploadSize);
    }
    else {
        r = service_->upload(fileName, contentType, body);
    }
    if (r.fileId.empty()) {
        respond_plain(resp, 500, "Internal Server Error", "upload failed", false);
        return;
    }

    // 生成完整 URL：优先用 Host 头（方便你反向代理/域名部署时自动生成可访问链接）。
    const std::string host = get_header_or_empty(req, "Host");

    std::string url;
    if (!host.empty()) {
        url = std::string("http://") + host + r.urlPath;
    }
    else {
        url = r.urlPath;
    }

    std::string json = std::string("{\"id\":\"") + filelink::json_escape_minimal(r.fileId) +
        "\",\"url\":\"" + filelink::json_escape_minimal(url) + "\"}";

    respond_json(resp, 200, "OK", json, true);
}

bool FileLinkServer::parse_file_id_from_path(const std::string& path, std::string& outFileId) {
    const std::string prefix = "/file/";
    if (path.find(prefix) != 0) {
        return false;
    }

    std::string id = path.substr(prefix.size());
    if (id.empty()) {
        return false;
    }

    // 简单防止目录穿越/非法字符：
    // 这里的 fileId 会最终变成落盘文件名（rootDir/fileId），因此要禁止 '/' 和 '..'。
    if (id.find('/') != std::string::npos || id.find("..") != std::string::npos) {
        return false;
    }

    outFileId = id;
    return true;
}

void FileLinkServer::handle_download(const HttpRequest& req, HttpResponse& resp) {
    // prefix 路由不区分 method，这里保持 HTTP 语义：非 GET 直接 405。
    if (req.get_method() != "GET") {
        respond_plain(resp, 405, "Method Not Allowed", "Method Not Allowed", false);
        resp.add_header("Allow", "GET");
        return;
    }

    std::string fileId;
    if (!parse_file_id_from_path(req.get_path(), fileId)) {
        respond_plain(resp, 400, "Bad Request", "bad file id", false);
        return;
    }

    // 下载流程：
    // 1) meta（cache 命中/回源 store）
    // 2) 按 meta.storagePath 读取文件内容
    DownloadResult out;
    if (!service_->download(fileId, out)) {
        respond_plain(resp, 404, "Not Found", "Not Found", true);
        return;
    }

    resp.set_status(200, "OK");
    resp.set_body(out.content);

    const std::string ct = !out.meta.contentType.empty()
        ? out.meta.contentType
        : filelink::guess_content_type_by_name(out.meta.originalName);
    resp.add_header("Content-Type", ct);

    // 触发下载，保留原始文件名
    resp.add_header("Content-Disposition", "attachment; filename=\"" + out.meta.originalName + "\"");

    set_keep_alive(resp, true);
}
