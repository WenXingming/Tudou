/**
 * @file ConfigLoader.h
 * @brief StaticFileHttpServer 配置加载：命令行解析 + 配置文件读取
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <string>

#include "StaticFileHttpServer.h"

struct StaticFileServerBootstrap {
    StaticFileServerConfig cfg;
    std::string serverRoot;   // ends with '/'
    std::string configPath;   // {serverRoot}conf/server.conf
    std::string logPath;      // {serverRoot}log/server.log
};

// Loads serverRoot + server.conf and fills cfg.
// - If "-r <serverRoot>" is provided: uses that.
// - Else: searches a few default roots.
// Returns true on success; on failure returns false and sets outError.
bool load_static_server_bootstrap(int argc, char* argv[],
                                  StaticFileServerBootstrap& out,
                                  std::string& outError);
