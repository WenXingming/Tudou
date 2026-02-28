/**
 * @file ConfigLoader.cpp
 * @brief StarMind 配置加载实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "ConfigLoader.h"

#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using ConfigMap = std::map<std::string, std::string>;

std::string trim(const std::string& str) {
    const size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

ConfigMap load_kv_config(const std::string& filename) {
    ConfigMap config;
    std::ifstream file(filename.c_str());
    if (!file.is_open()) return config;

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

bool is_option(const std::string& arg) {
    return !arg.empty() && arg[0] == '-';
}

// ---------------------------------------------------------------------------
// Config value accessors
// ---------------------------------------------------------------------------

bool parse_bool(const ConfigMap& cfg, const std::string& key, bool defaultValue) {
    auto it = cfg.find(key);
    if (it == cfg.end()) return defaultValue;

    std::string v = it->second;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] >= 'A' && v[i] <= 'Z') v[i] = static_cast<char>(v[i] - 'A' + 'a');
    }
    return (v == "1" || v == "true" || v == "yes" || v == "on");
}

std::string get_string_or(const ConfigMap& cfg, const std::string& key, const std::string& def) {
    auto it = cfg.find(key);
    return it == cfg.end() ? def : it->second;
}

int get_int_or(const ConfigMap& cfg, const std::string& key, int def) {
    auto it = cfg.find(key);
    if (it == cfg.end()) return def;
    try { return std::stoi(it->second); }
    catch (...) { return def; }
}

uint16_t get_u16_or(const ConfigMap& cfg, const std::string& key, uint16_t def) {
    const int v = get_int_or(cfg, key, static_cast<int>(def));
    return (v >= 0 && v <= 65535) ? static_cast<uint16_t>(v) : def;
}

std::string resolve_path(const std::string& serverRoot, const std::string& configuredPath) {
    if (configuredPath.empty()) return configuredPath;
    if (configuredPath[0] == '/') return configuredPath;
    return serverRoot + configuredPath;
}

// ---------------------------------------------------------------------------
// Command-line parsing
// ---------------------------------------------------------------------------

bool try_parse_server_root_from_args(int argc, char* argv[],
    std::string& outRoot,
    std::string& outError) {
    outRoot.clear();
    outError.clear();
    if (argc <= 1 || argv == nullptr) return false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = (argv[i] ? std::string(argv[i]) : std::string());

        if (a == "-r" || a == "--root") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                outError = "Missing value for -r/--root. Usage: StarMind -r <serverRoot>";
                return false;
            }
            outRoot = argv[i + 1];
            return true;
        }

        const std::string r1 = "-r=";
        const std::string r2 = "--root=";
        if (a.rfind(r1, 0) == 0) { outRoot = a.substr(r1.size()); return true; }
        if (a.rfind(r2, 0) == 0) { outRoot = a.substr(r2.size()); return true; }
    }

    // Backward-compatible: if argv[1] is a non-option, treat it as serverRoot.
    if (argv[1] != nullptr) {
        const std::string first = argv[1];
        if (!first.empty() && !is_option(first)) {
            outRoot = first;
            return true;
        }
    }

    return false;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

bool load_starmind_config(int argc, char* argv[],
    StarMindServerConfig& out,
    std::string& outError) {
    outError.clear();
    out = StarMindServerConfig{};

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
            "/etc/starmind/",
            "./starmind/",
            "./",
            "/home/wxm/Tudou/configs/starmind/",
        };
        for (const auto& root : searchRoots) {
            const std::string checkRoot = normalize_server_root(root);
            if (file_exists(checkRoot + "conf/server.conf")) {
                serverRoot = checkRoot;
                break;
            }
        }
        if (serverRoot.empty()) {
            outError =
                "No serverRoot and configuration found. Specify -r <serverRoot> (or argv[1]), "
                "or create conf/server.conf under:\n"
                "  /etc/starmind/\n"
                "  ./starmind/\n"
                "  ./\n"
                "  /home/wxm/Tudou/configs/starmind/\n";
            return false;
        }
    }

    serverRoot = normalize_server_root(serverRoot);

    // 3. 加载配置文件
    const std::string configPath = serverRoot + "conf/server.conf";
    const auto config = load_kv_config(configPath);
    if (config.empty()) {
        outError = "Could not load config or config is empty: " + configPath;
        return false;
    }

    // 4. 填充 StarMindServerConfig
    out.ip = get_string_or(config, "ip", "0.0.0.0");
    out.port = get_u16_or(config, "port", 8090);
    out.threadNum = get_int_or(config, "threadNum", 1);

    out.webRoot = resolve_path(serverRoot, get_string_or(config, "webRoot", serverRoot + "html/"));
    out.indexFile = get_string_or(config, "indexFile", "login.html");

    out.authEnabled = parse_bool(config, "auth.enabled", true);
    out.authUser = get_string_or(config, "auth.user", "admin");
    out.authPassword = get_string_or(config, "auth.password", "admin");
    out.authTokenTtlSeconds = get_int_or(config, "auth.token_ttl_seconds", 86400);

    out.llmProvider = get_string_or(config, "llm.provider", "openai_compat");
    out.llmApiBase = get_string_or(config, "llm.api_base", "https://api.deepseek.com/v1");
    out.llmApiKey = get_string_or(config, "llm.api_key", "");
    out.llmModel = get_string_or(config, "llm.model", "deepseek-chat");
    out.llmTimeoutSeconds = get_int_or(config, "llm.timeout_seconds", 60);
    out.llmSystemPrompt = get_string_or(config, "llm.system_prompt", "You are StarMind, a helpful assistant.");
    out.llmMaxHistoryMessages = get_int_or(config, "llm.max_history_messages", 20);

    out.serverRoot = serverRoot;
    out.configPath = configPath;
    out.logPath = serverRoot + "log/server.log";
    return true;
}
