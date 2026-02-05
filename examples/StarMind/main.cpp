#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "StarMindServer.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace {

using ConfigMap = std::map<std::string, std::string>;

std::string trim(const std::string& str) {
    const size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

ConfigMap load_kv_config(const std::string& filename) {
    ConfigMap configs;
    std::ifstream file(filename.c_str());
    if (!file.is_open()) {
        return configs;
    }

    std::string line;
    while (std::getline(file, line)) {
        const size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = trim(line);
        if (line.empty()) continue;

        const size_t delimiterPos = line.find('=');
        if (delimiterPos == std::string::npos) continue;

        const std::string key = trim(line.substr(0, delimiterPos));
        const std::string value = trim(line.substr(delimiterPos + 1));
        if (!key.empty()) {
            configs[key] = value;
        }
    }
    return configs;
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
    try {
        return std::stoi(it->second);
    }
    catch (...) {
        return def;
    }
}

uint16_t get_u16_or(const ConfigMap& cfg, const std::string& key, uint16_t def) {
    const int v = get_int_or(cfg, key, static_cast<int>(def));
    if (v < 0 || v > 65535) return def;
    return static_cast<uint16_t>(v);
}

std::string resolve_path(const std::string& serverRoot, const std::string& configuredPath) {
    if (configuredPath.empty()) return configuredPath;
    if (!configuredPath.empty() && configuredPath[0] == '/') return configuredPath;
    return serverRoot + configuredPath;
}

struct StarMindBootstrap {
    StarMindServerConfig cfg;
    std::string serverRoot;
    std::string configPath;
    std::string logPath;
};

bool try_parse_server_root_from_args(int argc, char* argv[], std::string& outServerRoot, std::string& outError) {
    outServerRoot.clear();
    outError.clear();

    if (argc <= 1 || argv == nullptr) return false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = (argv[i] ? std::string(argv[i]) : std::string());

        if (a == "-r" || a == "--root") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                outError = "Missing value for -r/--root. Usage: StarMind -r <serverRoot>";
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

bool load_starmind_server_bootstrap(int argc, char* argv[], StarMindBootstrap& out, std::string& outError) {
    outError.clear();
    out = StarMindBootstrap{};

    std::string serverRoot;
    std::string argError;
    const bool hasRootArg = try_parse_server_root_from_args(argc, argv, serverRoot, argError);
    if (!argError.empty()) {
        outError = std::move(argError);
        return false;
    }

    if (!hasRootArg) {
        const std::vector<std::string> searchRoots = {
            "/etc/starmind/",
            "./starmind/",
            "./",
            "/home/wxm/Tudou/configs/starmind/",
        };

        for (const auto& root : searchRoots) {
            const std::string checkRoot = normalize_server_root(root);
            const std::string checkPath = checkRoot + "conf/server.conf";
            if (file_exists(checkPath)) {
                serverRoot = checkRoot;
                break;
            }
        }

        if (serverRoot.empty()) {
            outError =
                "No serverRoot and configuration found. Specify -r <serverRoot> (or argv[1]), or create conf/server.conf under: "
                "/etc/starmind/, ./starmind/, ./, /home/wxm/Tudou/configs/starmind/.";
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

    StarMindServerConfig cfg;
    cfg.ip = get_string_or(config, "ip", "0.0.0.0");
    cfg.port = get_u16_or(config, "port", 8090);
    cfg.threadNum = get_int_or(config, "threadNum", 1);

    cfg.webRoot = resolve_path(serverRoot, get_string_or(config, "webRoot", serverRoot + "html/"));
    cfg.indexFile = get_string_or(config, "indexFile", "login.html");

    cfg.authEnabled = parse_bool(config, "auth.enabled", true);
    cfg.authUser = get_string_or(config, "auth.user", "admin");
    cfg.authPassword = get_string_or(config, "auth.password", "admin");
    cfg.authTokenTtlSeconds = get_int_or(config, "auth.token_ttl_seconds", 86400);

    cfg.llmProvider = get_string_or(config, "llm.provider", "openai_compat");
    cfg.llmApiBase = get_string_or(config, "llm.api_base", "https://api.deepseek.com/v1");
    cfg.llmApiKey = get_string_or(config, "llm.api_key", "");
    cfg.llmModel = get_string_or(config, "llm.model", "deepseek-chat");
    cfg.llmTimeoutSeconds = get_int_or(config, "llm.timeout_seconds", 60);
    cfg.llmSystemPrompt = get_string_or(config, "llm.system_prompt", "You are StarMind, a helpful assistant.");
    cfg.llmMaxHistoryMessages = get_int_or(config, "llm.max_history_messages", 20);

    out.cfg = std::move(cfg);
    out.serverRoot = serverRoot;
    out.configPath = configPath;
    out.logPath = serverRoot + "log/server.log";
    return true;
}

void set_logger(const std::string& logPath) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true));
    }
    catch (...) {
        // ignore file logger failures
    }

    auto logger = std::make_shared<spdlog::logger>("starmind", sinks.begin(), sinks.end());
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] [thread %t] %v");
}

} // namespace

int main(int argc, char* argv[]) {
    StarMindBootstrap bootstrap;
    std::string error;
    if (!load_starmind_server_bootstrap(argc, argv, bootstrap, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    set_logger(bootstrap.logPath);

    std::cout << "StarMind started: http://" << bootstrap.cfg.ip << ":" << bootstrap.cfg.port << std::endl;
    std::cout << "Login page:  GET  /login" << std::endl;
    std::cout << "Chat page:   GET  /chat" << std::endl;
    std::cout << "Login API:   POST /api/login" << std::endl;
    std::cout << "Chat API:    POST /api/chat" << std::endl;

    StarMindServer server(std::move(bootstrap.cfg));

    server.start();
    return 0;
}
