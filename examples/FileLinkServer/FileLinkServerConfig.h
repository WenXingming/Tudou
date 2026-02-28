/**
 * @file FileLinkServerConfig.h
 * @brief FileLinkServer 配置数据定义
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <cstdint>
#include <string>

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

    // SSL/HTTPS 配置
    bool sslEnabled = false;
    std::string sslCertFile = "";
    std::string sslKeyFile = "";

    // Paths resolved by ConfigLoader
    std::string serverRoot;   // ends with '/'
    std::string configPath;   // {serverRoot}conf/server.conf
    std::string logPath;      // {serverRoot}log/server.log
};
