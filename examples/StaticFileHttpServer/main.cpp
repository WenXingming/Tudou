/**
 * @file main.cpp
 * @brief http server 示例程序
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include <iostream>
#include <cassert>
#include <thread>
#include <unistd.h>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>

#include "StaticFileHttpServer.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h" // support for basic file logging
#include "spdlog/sinks/stdout_color_sinks.h" // support for colored console logging

// ======================================================================================
// Helper to trim strings
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) {
        return str;
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Config loader
std::map<std::string, std::string> load_config(const std::string& filename) {
    std::map<std::string, std::string> config;
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open config file " << filename << std::endl;
        return config;
    }
    std::string line;
    while (std::getline(file, line)) {
        // Remove comments
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

// 设置并注册 logger
void set_logger(const std::string& logPath) {
    // 创建 sinks 列表
    std::vector<spdlog::sink_ptr> sinks;

    // 添加控制台 sink（带颜色）
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    // 添加文件 sink（覆盖模式）
    // sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/tudou_test.log", true));
    // 添加文件 sink（追加模式）
    // sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("/home/wxm/Tudou/logs/tudou_test.log", false));
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true));

    // 创建组合 logger
    auto my_logger = std::make_shared<spdlog::logger>("multi_sink_logger", begin(sinks), end(sinks));

    // 注册并设为默认 logger
    spdlog::register_logger(my_logger);
    spdlog::set_default_logger(my_logger);

    // 设置日志级别和格式: debug、info、warn、err、critical 等
    spdlog::set_level(spdlog::level::err); // 设置全局日志级别
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] [thread %t] %v"); // 设置日志格式
}

void run_static_http_server(const std::map<std::string, std::string>& config) {
    std::string ip = config.count("ip") ? config.at("ip") : "0.0.0.0"; // 监听所有网卡地址，方便通过局域网其他设备访问测试
    int port = config.count("port") ? std::stoi(config.at("port")) : 8080; // 浏览器默认端口是 80（http）和 443（https），这里使用 8080 避免权限问题（ <1024 端口需要 root 权限 ）。实际部署时可使用 nginx 做反向代理转发到 8080（监听 80/443 端口，把请求转发到后端 8080）。
    std::string baseDir = config.count("baseDir") ? config.at("baseDir") : "./html/"; // 静态文件根目录
    int threadNum = config.count("threadNum") ? std::stoi(config.at("threadNum")) : 0; // 0 表示使用单线程，大于 0 表示使用多线程

    std::cout << "Config: ip=" << ip << ", port=" << port << ", baseDir=" << baseDir << ", threadNum=" << threadNum << std::endl;

    StaticFileHttpServer server(ip, static_cast<uint16_t>(port), baseDir, threadNum);
    server.start();
}

int main(int argc, char* argv[]) {

    std::string serverRoot;
    if (argc > 1) {
        serverRoot = argv[1];
    } else {
        // Default search roots
        std::vector<std::string> searchRoots = {
            "/home/wxm/Tudou/assets/static-file-http-server/",
            "/etc/static-file-http-server/",
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
                      << "   /home/wxm/Tudou/assets/static-file-http-server/\n"
                      << "   /etc/static-file-http-server/\n"
                      << "   ./static-file-http-server/\n"
                      << "2. Specify the server root directory as a command-line argument when running the program.\n";
            return 1;
        }
    }
    // Normalize serverRoot
    if (!serverRoot.empty() && serverRoot.back() != '/') {
        serverRoot += '/';
    }
    std::cout << "Correctly found Server Root: " << serverRoot << std::endl;

    std::string configPath = serverRoot + "conf/server.conf";
    std::cout << "Loading configuration from: " << configPath << std::endl;
    auto config = load_config(configPath);

    std::string logPath = serverRoot + "log/server.log";
    std::cout << "Log path set to: " << logPath << std::endl;

    std::string baseDir = serverRoot + "html/";
    std::cout << "Static file base directory set to: " << baseDir << std::endl;
    config["baseDir"] = baseDir; // Update config for run_static_http_server

    set_logger(logPath);
    run_static_http_server(config);

    return 0;
}
