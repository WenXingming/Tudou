#pragma once

#include <memory>
#include <string>

#include "tudou/http/HttpServer.h"
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
    FileLinkServer(FileLinkServerConfig cfg,
                   std::shared_ptr<IFileMetaStore> metaStore,
                   std::shared_ptr<IFileMetaCache> metaCache);

    void start();

private:
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
};
