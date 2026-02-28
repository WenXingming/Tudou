/**
 * @file StaticFileServerConfig.h
 * @brief StaticFileHttpServer 配置数据定义
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <cstdint>
#include <string>

struct StaticFileServerConfig {
    std::string ip        = "0.0.0.0";
    uint16_t    port      = 80;
    int         threadNum = 0;
    std::string baseDir   = "./assets/";

    // Paths resolved by ConfigLoader
    std::string serverRoot;   // ends with '/'
    std::string configPath;   // {serverRoot}conf/server.conf
    std::string logPath;      // {serverRoot}log/server.log
};
