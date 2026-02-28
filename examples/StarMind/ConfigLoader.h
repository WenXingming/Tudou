/**
 * @file ConfigLoader.h
 * @brief StarMind 配置加载：命令行解析 + 配置文件读取
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <string>

#include "StarMindConfig.h"

 // Loads serverRoot + server.conf and fills cfg.
 // - If "-r <serverRoot>" or "--root <serverRoot>" is provided: uses that.
 // - Else if argv[1] is a non-option: treats it as serverRoot.
 // - Else: searches a few default roots.
 // Returns true on success; on failure returns false and sets outError.
bool load_starmind_config(int argc, char* argv[],
    StarMindServerConfig& out,
    std::string& outError);
