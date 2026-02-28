/**
 * @file main.cpp
 * @brief StaticFileHttpServer 示例程序入口
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <iostream>
#include <string>
#include <vector>

#include "ConfigLoader.h"
#include "StaticFileHttpServer.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

static void set_logger(const std::string& logPath) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true));
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Warning: failed to create file logger at " << logPath << ": " << ex.what() << std::endl;
    }

    auto logger = std::make_shared<spdlog::logger>("static_server", sinks.begin(), sinks.end());
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::critical);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] [thread %t] %v");
}

int main(int argc, char* argv[]) {
    StaticFileServerBootstrap bootstrap;
    std::string error;
    if (!load_static_server_bootstrap(argc, argv, bootstrap, error)) {
        std::cerr << error << std::endl;
        return 1;
    }

    std::cout << "============================================================================================" << std::endl;
    std::cout << "Server root: " << bootstrap.serverRoot << std::endl;
    std::cout << "Config:      " << bootstrap.configPath << std::endl;
    std::cout << "Log:         " << bootstrap.logPath << std::endl;
    std::cout << "Base dir:    " << bootstrap.cfg.baseDir << std::endl;
    std::cout << "============================================================================================" << std::endl;

    set_logger(bootstrap.logPath);

    std::cout << "Serving static files from: " << bootstrap.cfg.baseDir << std::endl;
    std::cout << "Thread number (sub reactor threads): " << bootstrap.cfg.threadNum << std::endl;
    std::cout << "Server is running at http://" << bootstrap.cfg.ip << ":" << bootstrap.cfg.port << "/" << std::endl;

    StaticFileHttpServer server(std::move(bootstrap.cfg));
    server.start();
    return 0;
}
