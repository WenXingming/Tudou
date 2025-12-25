#include "FileLinkServer.h"

#include <fstream>
#include <sstream>

#include "spdlog/spdlog.h"

#include "util/HttpUtil.h"

FileLinkServer::FileLinkServer(FileLinkServerConfig cfg,
                               std::shared_ptr<IFileMetaStore> metaStore,
                               std::shared_ptr<IFileMetaCache> metaCache)
    : cfg_(std::move(cfg)),
      storage_(cfg_.storageRoot),
      httpServer_(nullptr),
      service_(nullptr) {

    httpServer_.reset(new HttpServer(cfg_.ip, cfg_.port, cfg_.threadNum));

    // 注意：storage_ 这里传给 service 是“按值拷贝/移动”的简单对象，
    // 这个 demo 里 FileSystemStorage 只持有 rootDir 字符串，所以拷贝成本很低。
    service_.reset(new FileLinkService(storage_, std::move(metaStore), std::move(metaCache)));

    httpServer_->set_http_callback(
        [this](const HttpRequest& req, HttpResponse& resp) {
            on_http_request(req, resp);
        });
}

void FileLinkServer::start() {
    spdlog::info("FileLinkServer listening on {}:{} storageRoot={} threadNum={}",
                cfg_.ip, cfg_.port, cfg_.storageRoot, cfg_.threadNum);
    httpServer_->start();
}

void FileLinkServer::on_http_request(const HttpRequest& req, HttpResponse& resp) {
    const std::string& method = req.get_method();
    const std::string& path = req.get_path();

    spdlog::debug("FileLinkServer: method={}, path={}", method, path);

    if (path == "/health" && method == "GET") {
        handle_health(req, resp);
        return;
    }

    // 最小前后端打通：直接由同一进程提供首页（省掉 Nginx/前端工程的依赖）。
    if ((path == "/" || path == "/homepage.html" || path == "/index.html") && method == "GET") {
        handle_index(req, resp);
        return;
    }

    if (path == "/upload" && method == "POST") {
        handle_upload(req, resp);
        return;
    }

    if (path.find("/file/") == 0 && method == "GET") {
        handle_download(req, resp);
        return;
    }

    resp.set_status(404, "Not Found");
    resp.set_body("Not Found");
    resp.add_header("Content-Type", "text/plain; charset=utf-8");
    resp.set_close_connection(true);
}

void FileLinkServer::handle_health(const HttpRequest& /*req*/, HttpResponse& resp) {
    resp.set_status(200, "OK");
    resp.set_body("OK");
    resp.add_header("Content-Type", "text/plain; charset=utf-8");
    resp.set_close_connection(false);
    resp.add_header("Connection", "Keep-Alive");
}

void FileLinkServer::handle_index(const HttpRequest& /*req*/, HttpResponse& resp) {
    if (cfg_.webRoot.empty()) {
        resp.set_status(404, "Not Found");
        resp.set_body("Not Found");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(false);
        resp.add_header("Connection", "Keep-Alive");
        return;
    }

    std::string path = cfg_.webRoot;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path += cfg_.indexFile;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        resp.set_status(500, "Internal Server Error");
        resp.set_body("index not found");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(true);
        return;
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();

    resp.set_status(200, "OK");
    resp.set_body(oss.str());
    resp.add_header("Content-Type", "text/html; charset=utf-8");
    resp.set_close_connection(false);
    resp.add_header("Connection", "Keep-Alive");
}

void FileLinkServer::handle_upload(const HttpRequest& req, HttpResponse& resp) {
    // 约定（为了最小可用，这里刻意不做 multipart/form-data）：
    //  - Body: 文件原始二进制内容
    //  - Header: X-File-Name: 原始文件名（可选）
    //  - Header: Content-Type: 可选
    // 注：multipart/form-data 可后续扩展，这里先做最小可用版本

    // 后端限制：最大 5GB（前端也会限制，但后端必须兜底）。
    static constexpr size_t kMaxUploadBytes = 5ULL * 1024 * 1024 * 1024;

    // 小文件：仍然允许走“内存 body”路径；大文件：HTTP 层会把 body 流式写到临时文件，并通过 header 传入路径。
    std::string tempUploadPath;
    int64_t tempUploadSize = -1;
    try {
        tempUploadPath = req.get_header("X-Temp-Upload-Path");
    } catch (...) {
        tempUploadPath.clear();
    }
    try {
        const std::string s = req.get_header("X-Temp-Upload-Size");
        tempUploadSize = static_cast<int64_t>(std::stoll(s));
    } catch (...) {
        tempUploadSize = -1;
    }

    const std::string& body = req.get_body();
    if (tempUploadPath.empty()) {
        if (body.empty()) {
            resp.set_status(400, "Bad Request");
            resp.set_body("empty body");
            resp.add_header("Content-Type", "text/plain; charset=utf-8");
            resp.set_close_connection(true);
            return;
        }
        if (body.size() > kMaxUploadBytes) {
            resp.set_status(413, "Payload Too Large");
            resp.set_body("payload too large (max 5GB)");
            resp.add_header("Content-Type", "text/plain; charset=utf-8");
            resp.set_close_connection(true);
            return;
        }
    } else {
        if (tempUploadSize > static_cast<int64_t>(kMaxUploadBytes)) {
            resp.set_status(413, "Payload Too Large");
            resp.set_body("payload too large (max 5GB)");
            resp.add_header("Content-Type", "text/plain; charset=utf-8");
            resp.set_close_connection(true);
            return;
        }
    }

    std::string fileName;
    try {
        fileName = filelink::url_decode(req.get_header("X-File-Name"));
    } catch (...) {
        fileName = "";
    }

    std::string contentType;
    try {
        contentType = req.get_header("Content-Type");
    } catch (...) {
        contentType = "";
    }

    // 业务编排交给 service：
    // - 生成 fileId
    // - 落盘
    // - 写入元数据 + cache
    UploadResult r;
    if (!tempUploadPath.empty()) {
        r = service_->upload_from_path(fileName, contentType, tempUploadPath, tempUploadSize);
    } else {
        r = service_->upload(fileName, contentType, body);
    }
    if (r.fileId.empty()) {
        resp.set_status(500, "Internal Server Error");
        resp.set_body("upload failed");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(true);
        return;
    }

    // 生成完整 URL：优先用 Host 头（方便你反向代理/域名部署时自动生成可访问链接）。
    std::string host;
    try {
        host = req.get_header("Host");
    } catch (...) {
        host = "";
    }

    std::string url;
    if (!host.empty()) {
        url = std::string("http://") + host + r.urlPath;
    } else {
        url = r.urlPath;
    }

    std::string json = std::string("{\"id\":\"") + filelink::json_escape_minimal(r.fileId) +
                       "\",\"url\":\"" + filelink::json_escape_minimal(url) + "\"}";

    resp.set_status(200, "OK");
    resp.set_body(json);
    resp.add_header("Content-Type", "application/json; charset=utf-8");
    resp.set_close_connection(false);
    resp.add_header("Connection", "Keep-Alive");
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
    std::string fileId;
    if (!parse_file_id_from_path(req.get_path(), fileId)) {
        resp.set_status(400, "Bad Request");
        resp.set_body("bad file id");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(true);
        return;
    }

    // 下载流程：
    // 1) meta（cache 命中/回源 store）
    // 2) 按 meta.storagePath 读取文件内容
    DownloadResult out;
    if (!service_->download(fileId, out)) {
        resp.set_status(404, "Not Found");
        resp.set_body("Not Found");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(false);
        resp.add_header("Connection", "Keep-Alive");
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

    resp.set_close_connection(false);
    resp.add_header("Connection", "Keep-Alive");
}
