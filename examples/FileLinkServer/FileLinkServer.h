/**
 * @file FileLinkServer.h
 * @brief HTTP 服务器实现
 * @details 负责 HTTP 路由与协议细节，调用 FileLinkService 完成具体业务逻辑。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <memory>
#include <string>

#include "auth/AuthService.h"

#include "tudou/http/HttpServer.h"
#include "tudou/router/Router.h"
#include "FileLinkService.h"

 // Date Class for configure file
struct FileLinkServerConfig {
    std::string ip = "0.0.0.0";
    uint16_t port = 8080;
    int threadNum = 0;

    std::string storageRoot = "./filelink_storage";

    // 用于前后端最小打通：FileLinkServer 直接返回一个静态首页
    std::string webRoot = ""; // 为空表示不提供静态页面
    std::string indexFile = "index.html";

    // Auth (simple demo): require login to upload
    bool authEnabled = false;
    std::string authUser = "";
    std::string authPassword = "";
    int authTokenTtlSeconds = 3600;

    // MySQL meta store
    bool mysqlEnabled = false;
    std::string mysqlHost = "127.0.0.1";
    int mysqlPort = 3306;
    std::string mysqlUser = "root";
    std::string mysqlPassword = "";
    std::string mysqlDatabase = "tudou_db";

    // Redis meta cache
    bool redisEnabled = false;
    std::string redisHost = "127.0.0.1";
    int redisPort = 6379;
};

class FileLinkServer {
public:
    // FileLinkServer 是“HTTP 适配层”：
    // - 负责路由、协议细节（Header/Status/JSON）
    // - 不做业务规则本身，业务交给 FileLinkService
    // 推荐：直接传 cfg，由 server 内部创建 metaStore/metaCache 并注入 service。
    explicit FileLinkServer(FileLinkServerConfig cfg);

    ~FileLinkServer();

    void start();

private:
    // 统一入口：所有 HTTP 请求都从这里分发到各 handler。
    void on_http_request(const HttpRequest& req, HttpResponse& resp);

    void init();

    void handle_static(const HttpRequest& req, HttpResponse& resp);
    bool resolve_static_real_path(const std::string& requestPath, std::string& outRealPath) const;
    bool read_file_all(const std::string& realPath, std::string& outBody) const;

    void handle_login(const HttpRequest& req, HttpResponse& resp);
    void handle_upload(const HttpRequest& req, HttpResponse& resp);
    void handle_download(const HttpRequest& req, HttpResponse& resp);

    static bool parse_file_id_from_path(const std::string& path, std::string& outFileId);

    bool require_auth(const HttpRequest& req, HttpResponse& resp);

private:
    FileLinkServerConfig cfg_;

    filelink::AuthService auth_;
    std::unique_ptr<FileLinkService> service_;

    std::unique_ptr<HttpServer> httpServer_;
    Router router_;


};
