/**
 * @file main.cpp
 * @brief FileLinkServer 示例程序（上传文件 -> 返回 URL；访问 URL -> 下载文件）
 */

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <map>

#include "FileLinkServer.h"
#include "meta/InMemoryFileMetaStore.h"
#include "meta/MysqlFileMetaStore.h"
#include "cache/NoopFileMetaCache.h"
#include "cache/RedisFileMetaCache.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

// ======================================================================================
// Helper to trim strings
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) {
        return str;
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Config loader
static std::map<std::string, std::string> load_config(const std::string& filename) {
    std::map<std::string, std::string> config;
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open config file " << filename << std::endl;
        return config;
    }
    std::string line;
    while (std::getline(file, line)) {
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = trim(line);
        if (line.empty()) continue;

        size_t delimiterPos = line.find('=');
        if (delimiterPos != std::string::npos) {
            std::string key = trim(line.substr(0, delimiterPos));
            std::string value = trim(line.substr(delimiterPos + 1));
            config[key] = value;
        }
    }
    return config;
}

static std::string resolve_path(const std::string& serverRoot, const std::string& configuredPath) {
    if (configuredPath.empty()) {
        return configuredPath;
    }
    if (!configuredPath.empty() && configuredPath[0] == '/') {
        return configuredPath;
    }
    return serverRoot + configuredPath;
}

static bool parse_bool(const std::map<std::string, std::string>& cfg,
                       const std::string& key,
                       bool defaultValue) {
    auto it = cfg.find(key);
    if (it == cfg.end()) return defaultValue;
    std::string v = it->second;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] >= 'A' && v[i] <= 'Z') v[i] = static_cast<char>(v[i] - 'A' + 'a');
    }
    return (v == "1" || v == "true" || v == "yes" || v == "on");
}

static void set_logger(const std::string& logPath) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true));

    auto logger = std::make_shared<spdlog::logger>("filelink", begin(sinks), end(sinks));
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] [thread %t] %v");
}

int main(int argc, char* argv[]) {
    // 最小可用版本：
    //  - 元数据：内存 map（你接入 MySQL 后替换为 MySQL store）
    //  - 缓存：Noop（你部署 Redis 后替换为 Redis cache）
    //  - 首页：从 serverRoot/html/{indexFile} 读取并由 FileLinkServer 直接返回

    std::string serverRoot;
    if (argc > 1) {
        serverRoot = argv[1];
    } else {
        std::vector<std::string> searchRoots = {
            "/home/wxm/Tudou/configs/file-link-server/",
            "/etc/file-link-server/",
            "./"
        };

        for (const auto& root : searchRoots) {
            std::string checkPath = root;
            if (!checkPath.empty() && checkPath.back() != '/') checkPath += '/';
            checkPath += "conf/server.conf";

            std::ifstream f(checkPath);
            if (f.good()) {
                serverRoot = root;
                break;
            }
        }

        if (serverRoot.empty()) {
            std::cout << "No serverRoot and configuration found in default locations. You have two options:\n"
                      << "1. Create a serverRoot folder at one of the default locations:\n"
                      << "   /home/wxm/Tudou/configs/file-link-server/\n"
                      << "   /etc/file-link-server/\n"
                      << "   ./file-link-server/\n"
                      << "2. Specify the server root directory as a command-line argument when running the program.\n";
            return 1;
        }
    }
    if (!serverRoot.empty() && serverRoot.back() != '/') {
        serverRoot += '/';
    }

    std::string configPath = serverRoot + "conf/server.conf";
    auto config = load_config(configPath);

    std::string logPath = serverRoot + "log/server.log";
    set_logger(logPath);

    FileLinkServerConfig cfg;
    cfg.ip = config.count("ip") ? config.at("ip") : "0.0.0.0";
    cfg.port = config.count("port") ? static_cast<uint16_t>(std::stoi(config.at("port"))) : 8080;
    cfg.threadNum = config.count("threadNum") ? std::stoi(config.at("threadNum")) : 4;
    cfg.storageRoot = config.count("storageRoot") ? resolve_path(serverRoot, config.at("storageRoot")) : (serverRoot + "storage/");
    cfg.webRoot = config.count("webRoot") ? resolve_path(serverRoot, config.at("webRoot")) : (serverRoot + "html/");
    cfg.indexFile = config.count("indexFile") ? config.at("indexFile") : "homepage.html";

    std::shared_ptr<IFileMetaStore> metaStore;
    std::shared_ptr<IFileMetaCache> metaCache;

    const bool mysqlEnabled = parse_bool(config, "mysql.enabled", false);
    const bool redisEnabled = parse_bool(config, "redis.enabled", false);

#if FILELINK_WITH_MYSQLCPPCONN
    if (mysqlEnabled) {
        const std::string host = config.count("mysql.host") ? config.at("mysql.host") : "127.0.0.1";
        const int port = config.count("mysql.port") ? std::stoi(config.at("mysql.port")) : 3306;
        const std::string user = config.count("mysql.user") ? config.at("mysql.user") : "root";
        const std::string password = config.count("mysql.password") ? config.at("mysql.password") : "";
        const std::string database = config.count("mysql.database") ? config.at("mysql.database") : "tudou_db";
        metaStore = std::make_shared<MysqlFileMetaStore>(host, port, user, password, database);
    } else {
        metaStore = std::make_shared<InMemoryFileMetaStore>();
    }
#else
    (void)mysqlEnabled;
    metaStore = std::make_shared<InMemoryFileMetaStore>();
    spdlog::warn("MySQL enabled in config but FileLinkServer was built without mysqlcppconn; falling back to InMemoryFileMetaStore.");
#endif

#if FILELINK_WITH_HIREDIS
    if (redisEnabled) {
        const std::string host = config.count("redis.host") ? config.at("redis.host") : "127.0.0.1";
        const int port = config.count("redis.port") ? std::stoi(config.at("redis.port")) : 6379;
        metaCache = std::make_shared<RedisFileMetaCache>(host, port);
    } else {
        metaCache = std::make_shared<NoopFileMetaCache>();
    }
#else
    (void)redisEnabled;
    metaCache = std::make_shared<NoopFileMetaCache>();
    spdlog::warn("Redis enabled in config but FileLinkServer was built without hiredis; falling back to NoopFileMetaCache.");
#endif

    FileLinkServer server(cfg, metaStore, metaCache);

    std::cout << "FileLinkServer started: http://" << cfg.ip << ":" << cfg.port << std::endl;
    std::cout << "Homepage: GET  /" << std::endl;
    std::cout << "Upload:    POST /upload (Header: X-File-Name)" << std::endl;
    std::cout << "Download:  GET  /file/{id}" << std::endl;

    server.start();
    return 0;
}
