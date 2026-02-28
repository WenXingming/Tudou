/**
 * @file ConfigLoader.cpp
 * @brief StaticFileHttpServer 配置加载实现
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include "ConfigLoader.h"

#include <cerrno>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

// ---------------------------------------------------------------------------
// 通用工具
// ---------------------------------------------------------------------------

std::string trim(const std::string& str) {
    const size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

bool file_exists(const std::string& path) {
    std::ifstream f(path.c_str());
    return f.good();
}

std::string normalize_server_root(std::string root) {
    if (!root.empty() && root.back() != '/') {
        root.push_back('/');
    }
    return root;
}

bool ensure_dir_recursive(const std::string& dir) {
    if (dir.empty()) {
        return true;
    }
    std::string current;
    size_t pos = 0;
    if (dir[0] == '/') {
        current = "/";
        pos = 1;
    }
    while (pos < dir.size()) {
        const size_t next = dir.find('/', pos);
        const std::string part = (next == std::string::npos)
            ? dir.substr(pos)
            : dir.substr(pos, next - pos);
        if (!part.empty()) {
            if (current.size() > 1 && current.back() != '/') {
                current.push_back('/');
            }
            current += part;
            if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Key-Value 配置文件解析
// ---------------------------------------------------------------------------

using ConfigMap = std::map<std::string, std::string>;

ConfigMap load_kv_config(const std::string& filename) {
    ConfigMap config;
    std::ifstream file(filename.c_str());
    if (!file.is_open()) {
        return config;
    }
    std::string line;
    while (std::getline(file, line)) {
        const size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = trim(line);
        if (line.empty()) continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (!key.empty()) {
            config[key] = value;
        }
    }
    return config;
}

std::string get_string_or(const ConfigMap& cfg, const std::string& key, const std::string& defaultValue) {
    auto it = cfg.find(key);
    return (it != cfg.end()) ? it->second : defaultValue;
}

int get_int_or(const ConfigMap& cfg, const std::string& key, int defaultValue) {
    auto it = cfg.find(key);
    if (it == cfg.end()) return defaultValue;
    try {
        std::string s = trim(it->second);
        size_t idx = 0;
        const int v = std::stoi(s, &idx);
        return (idx == s.size()) ? v : defaultValue;
    }
    catch (...) {
        return defaultValue;
    }
}

uint16_t get_u16_or(const ConfigMap& cfg, const std::string& key, uint16_t defaultValue) {
    const int v = get_int_or(cfg, key, static_cast<int>(defaultValue));
    return (v >= 0 && v <= 65535) ? static_cast<uint16_t>(v) : defaultValue;
}

// ---------------------------------------------------------------------------
// 命令行解析
// ---------------------------------------------------------------------------

bool try_parse_server_root_from_args(int argc, char* argv[],
    std::string& outRoot,
    std::string& outError) {
    outRoot.clear();
    outError.clear();
    if (argc <= 1 || argv == nullptr) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string a = (argv[i] != nullptr) ? std::string(argv[i]) : std::string();

        if (a == "-h" || a == "--help") {
            outError =
                "Usage:\n"
                "  StaticFileHttpServer -r <serverRoot>\n"
                "  StaticFileHttpServer -h\n\n"
                "serverRoot should contain: conf/server.conf, assets/, log/ ...\n";
            return false;
        }

        if (a == "-r" || a == "--root") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                outError = "Missing value for " + a + ". Usage: StaticFileHttpServer -r <serverRoot>";
                return false;
            }
            outRoot = argv[i + 1];
            return true;
        }
    }

    return false;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

bool load_static_server_bootstrap(int argc, char* argv[],
    StaticFileServerBootstrap& out,
    std::string& outError) {
    outError.clear();
    out = StaticFileServerBootstrap{};

    // 1. 尝试从命令行参数获取 serverRoot
    std::string serverRoot;
    std::string argError;
    const bool hasRootArg = try_parse_server_root_from_args(argc, argv, serverRoot, argError);
    if (!argError.empty()) {
        outError = std::move(argError);
        return false;
    }

    // 2. 如果命令行未指定，搜索默认位置
    if (!hasRootArg) {
        const std::vector<std::string> searchRoots = {
            "/etc/static-file-http-server/",
            "./static-file-http-server/",
            "./",
            "/home/wxm/Tudou/configs/static-file-http-server/",
        };
        for (const auto& root : searchRoots) {
            std::string checkRoot = normalize_server_root(root);
            if (file_exists(checkRoot + "conf/server.conf")) {
                serverRoot = checkRoot;
                break;
            }
        }
        if (serverRoot.empty()) {
            outError =
                "No configuration found in default locations.\n"
                "Specify server root with -r <serverRoot>, or create conf/server.conf under one of:\n"
                "  /etc/static-file-http-server/\n"
                "  ./static-file-http-server/\n"
                "  ./\n"
                "  /home/wxm/Tudou/configs/static-file-http-server/\n";
            return false;
        }
    }

    serverRoot = normalize_server_root(serverRoot);

    // 3. 校验并加载配置文件
    const std::string configPath = serverRoot + "conf/server.conf";
    if (!file_exists(configPath)) {
        outError = "Config not found: " + configPath;
        return false;
    }
    const ConfigMap config = load_kv_config(configPath);

    // 4. 确保日志目录存在
    const std::string logDir = serverRoot + "log/";
    if (!ensure_dir_recursive(logDir)) {
        std::cerr << "Warning: failed to ensure log directory: " << logDir << std::endl;
    }

    // 5. 填充输出
    StaticFileServerConfig cfg;
    cfg.ip = get_string_or(config, "ip", "0.0.0.0");
    cfg.port = get_u16_or(config, "port", 80);
    cfg.threadNum = get_int_or(config, "threadNum", 0);
    cfg.baseDir = serverRoot + "assets/";

    out.cfg = std::move(cfg);
    out.serverRoot = serverRoot;
    out.configPath = configPath;
    out.logPath = serverRoot + "log/server.log";
    return true;
}
