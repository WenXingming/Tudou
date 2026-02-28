/**
 * @file main.cpp
 * @brief StarMind 示例程序入口
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <iostream>
#include <string>
#include <vector>

#include "ConfigLoader.h"
#include "StarMindServer.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

static void set_logger(const std::string& logPath) {
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

int main(int argc, char* argv[]) {
    StarMindServerConfig cfg;
    std::string error;
    if (!load_starmind_config(argc, argv, cfg, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    set_logger(cfg.logPath);

    std::cout << "StarMind started: http://" << cfg.ip << ":" << cfg.port << std::endl;
    std::cout << "Login page:  GET  /login" << std::endl;
    std::cout << "Chat page:   GET  /chat" << std::endl;
    std::cout << "Login API:   POST /api/login" << std::endl;
    std::cout << "Chat API:    POST /api/chat" << std::endl;

    StarMindServer server(std::move(cfg));
    server.start();
    return 0;
}
