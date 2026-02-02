/**
 * @file main.cpp
 * @brief http server 示例程序
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include <cstdint>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "StaticFileHttpServer.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h" // support for basic file logging
#include "spdlog/sinks/stdout_color_sinks.h" // support for colored console logging

using ConfigMap = std::map<std::string, std::string>;

std::string trim(const std::string& str) {
    // 去除字符串首尾的空白字符、换行符等等
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

std::string ensure_trailing_slash(std::string path) {
    // 确保路径以 "/" 结尾
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    return path;
}

bool ensure_dir_recursive(const std::string& dir) {
    // 此函数确保目录存在，递归创建不存在的父目录
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

        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }

    return true;
}

ConfigMap load_config(const std::string& filename) {
    ConfigMap configs;
    // 如果配置文件未被找到，返回空配置
    std::ifstream file(filename.c_str());
    if (!file.is_open()) {
        std::cerr << "Error: Could not open config file: " << filename << std::endl;
        return configs;
    }
    // 逐行读取配置文件
    std::string line;
    while (std::getline(file, line)) {
        // 去除注释和多余空白
        const size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        // 解析 key=value 格式
        const size_t delimiterPos = line.find('=');
        if (delimiterPos == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, delimiterPos));
        const std::string value = trim(line.substr(delimiterPos + 1));
        if (!key.empty()) {
            configs[key] = value;
        }
    }
    return configs;
}

std::string get_string(const ConfigMap& configs, const std::string& key, const std::string& defaultValue) {
    // 获取字符串配置项
    const ConfigMap::const_iterator it = configs.find(key);
    return (it != configs.end()) ? it->second : defaultValue;
}

int get_int(const ConfigMap& configs, const std::string& key, int defaultValue) {
    // 获取整数配置项
    const ConfigMap::const_iterator it = configs.find(key);
    if (it == configs.end()) {
        return defaultValue;
    }

    try {
        std::string s = trim(it->second);
        size_t idx = 0;
        const int v = std::stoi(s, &idx);
        if (idx != s.size()) {
            return defaultValue;
        }
        return v;
    }
    catch (...) {
        return defaultValue;
    }
}

struct ServerPaths {
    std::string root;
    std::string configPath;
    std::string logDir;
    std::string logPath;
    std::string baseDir;
};

ServerPaths make_paths(std::string root) {
    root = ensure_trailing_slash(root);
    ServerPaths paths;
    paths.root = root;
    paths.configPath = root + "conf/server.conf";
    paths.logDir = root + "log/";
    paths.logPath = root + "log/server.log";
    paths.baseDir = root + "assets/";
    return paths;
}

std::string find_server_root(int argc, char* argv[]) {
    // 优先使用命令行参数指定的 serverRoot（root 目录，包含 conf/server.conf、html/、log/ 等）
    for (int i = 1; i < argc; ++i) {
        const std::string arg = (argv[i] != NULL) ? std::string(argv[i]) : std::string();

        if (arg == "-h" || arg == "--help") {
            return "";
        }

        if (arg == "-r") {
            if (i + 1 >= argc || argv[i + 1] == NULL) {
                std::cerr << "Error: missing value for " << arg << std::endl;
                return "";
            }
            const std::string root = ensure_trailing_slash(argv[i + 1]);
            const ServerPaths paths = make_paths(root);
            if (!file_exists(paths.configPath)) {
                std::cerr << "Error: config not found: " << paths.configPath << std::endl;
                return "";
            }
            return root;
        }
    }
    // 否则在默认位置搜索
    const std::vector<std::string> searchRoots = {
        "/etc/static-file-http-server/",
        "./static-file-http-server/",
        "./",
        "/home/wxm/Tudou/configs/static-file-http-server/",
    };

    for (size_t i = 0; i < searchRoots.size(); ++i) {
        const ServerPaths paths = make_paths(searchRoots[i]);
        if (file_exists(paths.configPath)) {
            return paths.root;
        }
    }

    return "";
}

void print_missing_root_help() {
    std::cout
        << "Usage:\n"
        << "  StaticFileHttpServer -r <serverRoot>\n"
        << "  StaticFileHttpServer -h\n\n"
        << "serverRoot should contain: conf/server.conf, assets/, log/ ...\n\n"
        << "No configuration found in default locations. You have two options:\n"
        << "1. Create a serverRoot folder at one of the default locations:\n"
        << "   /etc/static-file-http-server/\n"
        << "   ${path_of_the_executable}/static-file-http-server/\n"
        << "2. Specify the serverRoot directory via -r when running the program.\n";
}


// 设置并注册 logger
void set_logger(const std::string& logPath) {
    std::vector<spdlog::sink_ptr> sinks; // 创建多个 sink 用于多目标日志记录
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>()); // 控制台日志（彩色）

    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true)); // 文件日志。true 表示覆盖写入
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Warning: failed to create file logger at " << logPath << ": " << ex.what() << std::endl;
    }

    auto my_logger = std::make_shared<spdlog::logger>("multi_sink_logger", sinks.begin(), sinks.end());
    spdlog::register_logger(my_logger); // 注册 logger 以便全局访问
    spdlog::set_default_logger(my_logger); // 设置为默认 logger
    spdlog::set_level(spdlog::level::critical); // debug、info、warn、error、critical 等
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] [thread %t] %v"); // 设置日志输出格式
}

void run_static_http_server(const ConfigMap& config) {
    const std::string ip = get_string(config, "ip", "0.0.0.0");
    const int port = get_int(config, "port", 80);
    const std::string baseDir = get_string(config, "baseDir", "./assets/");
    const int threadNum = get_int(config, "threadNum", 0);

    if (port < 0 || port > 65535) {
        std::cerr << "Error: invalid port: " << port << " (expected 0-65535)" << std::endl;
        return;
    }

    std::cout << "Serving static files from: " << baseDir << std::endl;
    std::cout << "The thread number（sub reactor threads） is: " << threadNum << std::endl;
    std::cout << "Server is running at http://" << ip << ":" << port << "/" << std::endl;

    StaticFileHttpServer server(ip, static_cast<uint16_t>(port), baseDir, threadNum);
    server.start();
}

int main(int argc, char* argv[]) {

    const std::string serverRoot = find_server_root(argc, argv);
    if (serverRoot.empty()) {
        print_missing_root_help();
        return 1;
    }

    const ServerPaths paths = make_paths(serverRoot);
    std::cout << "============================================================================================" << std::endl;
    std::cout << "Server root: " << paths.root << std::endl;
    std::cout << "Loading configuration from: " << paths.configPath << std::endl;

    ConfigMap config = load_config(paths.configPath);
    config["baseDir"] = paths.baseDir;

    if (!ensure_dir_recursive(paths.logDir)) {
        std::cerr << "Warning: failed to ensure log directory: " << paths.logDir << std::endl;
    }
    std::cout << "Log path: " << paths.logPath << std::endl;
    std::cout << "Static file base directory: " << paths.baseDir << std::endl;
    std::cout << "============================================================================================" << std::endl;
    set_logger(paths.logPath);
    run_static_http_server(config);

    return 0;
}
