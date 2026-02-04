#include "ConfigLoader.h"

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return str;
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::map<std::string, std::string> load_kv_config(const std::string& filename) {
    std::map<std::string, std::string> config;
    std::ifstream file(filename);
    if (!file.is_open()) {
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
        if (delimiterPos == std::string::npos) continue;

        std::string key = trim(line.substr(0, delimiterPos));
        std::string value = trim(line.substr(delimiterPos + 1));
        config[key] = value;
    }

    return config;
}

std::string resolve_path(const std::string& serverRoot, const std::string& configuredPath) {
    if (configuredPath.empty()) {
        return configuredPath;
    }
    if (!configuredPath.empty() && configuredPath[0] == '/') {
        return configuredPath;
    }
    return serverRoot + configuredPath;
}

bool parse_bool(const std::map<std::string, std::string>& cfg,
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

std::string get_string_or(const std::map<std::string, std::string>& cfg,
    const std::string& key,
    const std::string& defaultValue) {
    auto it = cfg.find(key);
    if (it == cfg.end()) {
        return defaultValue;
    }
    return it->second;
}

int get_int_or(const std::map<std::string, std::string>& cfg,
    const std::string& key,
    int defaultValue) {
    auto it = cfg.find(key);
    if (it == cfg.end()) {
        return defaultValue;
    }
    try {
        return std::stoi(it->second);
    }
    catch (...) {
        return defaultValue;
    }
}

uint16_t get_u16_or(const std::map<std::string, std::string>& cfg,
    const std::string& key,
    uint16_t defaultValue) {
    const int v = get_int_or(cfg, key, static_cast<int>(defaultValue));
    if (v < 0) {
        return defaultValue;
    }
    if (v > 65535) {
        return defaultValue;
    }
    return static_cast<uint16_t>(v);
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::string normalize_server_root(std::string root) {
    if (!root.empty() && root.back() != '/') {
        root.push_back('/');
    }
    return root;
}

bool is_option(const std::string& arg) {
    return !arg.empty() && arg[0] == '-';
}

bool try_parse_server_root_from_args(int argc, char* argv[], std::string& outServerRoot, std::string& outError) {
    outServerRoot.clear();
    outError.clear();
    if (argc <= 1 || argv == nullptr) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string a = (argv[i] ? std::string(argv[i]) : std::string());

        if (a == "-r" || a == "--root") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                outError = "Missing value for " + a + ". Usage: filelink-server -r <serverRoot>";
                return false;
            }
            outServerRoot = argv[i + 1];
            return true;
        }

        const std::string r1 = "-r=";
        const std::string r2 = "--root=";
        if (a.rfind(r1, 0) == 0) {
            outServerRoot = a.substr(r1.size());
            return true;
        }
        if (a.rfind(r2, 0) == 0) {
            outServerRoot = a.substr(r2.size());
            return true;
        }
    }

    // Backward-compatible: if argv[1] is a non-option, treat it as serverRoot.
    if (argv[1] != nullptr) {
        const std::string first = argv[1];
        if (!first.empty() && !is_option(first)) {
            outServerRoot = first;
            return true;
        }
    }

    return false;
}

} // namespace

bool load_filelink_server_bootstrap(int argc, char* argv[], FileLinkServerBootstrap& out, std::string& outError) {
    outError.clear();
    out = FileLinkServerBootstrap{};

    std::string serverRoot;
    std::string argError;
    const bool hasRootArg = try_parse_server_root_from_args(argc, argv, serverRoot, argError);
    if (!argError.empty()) {
        outError = std::move(argError);
        return false;
    }

    if (!hasRootArg) {
        const std::vector<std::string> searchRoots = {
            "/etc/file-link-server/",
            "./file-link-server/",
            "./",
            "/home/wxm/Tudou/configs/file-link-server/",
        };

        for (const auto& root : searchRoots) {
            std::string checkRoot = normalize_server_root(root);
            const std::string checkPath = checkRoot + "conf/server.conf";
            if (file_exists(checkPath)) {
                serverRoot = checkRoot;
                break;
            }
        }

        if (serverRoot.empty()) {
            outError =
                "No serverRoot and configuration found in default locations. "
                "Specify server root with -r <serverRoot> (or as argv[1]), or create conf/server.conf under one of: "
                "/etc/file-link-server/, ./file-link-server/, ./, /home/wxm/Tudou/configs/file-link-server/.";
            return false;
        }
    }

    serverRoot = normalize_server_root(serverRoot);

    const std::string configPath = serverRoot + "conf/server.conf";
    const auto config = load_kv_config(configPath);
    if (config.empty()) {
        outError = "Could not load config or config is empty: " + configPath;
        return false;
    }

    FileLinkServerConfig cfg;
    cfg.ip = get_string_or(config, "ip", "0.0.0.0");
    cfg.port = get_u16_or(config, "port", 8080);
    cfg.threadNum = get_int_or(config, "threadNum", 4);

    cfg.storageRoot = resolve_path(serverRoot, get_string_or(config, "storageRoot", serverRoot + "storage/"));
    cfg.webRoot = resolve_path(serverRoot, get_string_or(config, "webRoot", serverRoot + "html/"));
    cfg.indexFile = get_string_or(config, "indexFile", "homepage.html");

    cfg.authEnabled = parse_bool(config, "auth.enabled", false);
    cfg.authUser = get_string_or(config, "auth.user", "");
    cfg.authPassword = get_string_or(config, "auth.password", "");
    cfg.authTokenTtlSeconds = get_int_or(config, "auth.token_ttl_seconds", 3600);

    cfg.mysqlEnabled = parse_bool(config, "mysql.enabled", false);
    cfg.mysqlHost = get_string_or(config, "mysql.host", "127.0.0.1");
    cfg.mysqlPort = get_int_or(config, "mysql.port", 3306);
    cfg.mysqlUser = get_string_or(config, "mysql.user", "root");
    cfg.mysqlPassword = get_string_or(config, "mysql.password", "");
    cfg.mysqlDatabase = get_string_or(config, "mysql.database", "tudou_db");

    cfg.redisEnabled = parse_bool(config, "redis.enabled", false);
    cfg.redisHost = get_string_or(config, "redis.host", "127.0.0.1");
    cfg.redisPort = get_int_or(config, "redis.port", 6379);

    out.cfg = std::move(cfg);
    out.serverRoot = serverRoot;
    out.configPath = configPath;
    out.logPath = serverRoot + "log/server.log";
    return true;
}
