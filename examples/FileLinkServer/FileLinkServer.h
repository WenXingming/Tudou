#pragma once

#include <memory>
#include <string>

#include "tudou/http/HttpServer.h"
#include "tudou/router/Router.h"
#include "FileLinkService.h"

struct FileLinkServerConfig {
    std::string ip = "0.0.0.0";
    uint16_t port = 8080;
    int threadNum = 0;

    std::string storageRoot = "./filelink_storage";

    // 用于前后端最小打通：FileLinkServer 直接返回一个静态首页
    std::string webRoot = ""; // 为空表示不提供静态页面
    std::string indexFile = "index.html";
};

class FileLinkServer {
public:
    // FileLinkServer 是“HTTP 适配层”：
    // - 负责路由、协议细节（Header/Status/JSON）
    // - 不做业务规则本身，业务交给 FileLinkService
    // - 通过依赖注入把 metaStore/metaCache 换成不同实现
    FileLinkServer(FileLinkServerConfig cfg,
                   std::shared_ptr<IFileMetaStore> metaStore,
                   std::shared_ptr<IFileMetaCache> metaCache);

    void start();

private:
    // 统一入口：所有 HTTP 请求都从这里分发到各 handler。
    void on_http_request(const HttpRequest& req, HttpResponse& resp);

    void handle_health(const HttpRequest& req, HttpResponse& resp);
    void handle_index(const HttpRequest& req, HttpResponse& resp);
    void handle_upload(const HttpRequest& req, HttpResponse& resp);
    void handle_download(const HttpRequest& req, HttpResponse& resp);

    static bool parse_file_id_from_path(const std::string& path, std::string& outFileId);

private:
    FileLinkServerConfig cfg_;

    FileSystemStorage storage_;
    std::unique_ptr<HttpServer> httpServer_;
    std::unique_ptr<FileLinkService> service_;

    Router router_;
};
