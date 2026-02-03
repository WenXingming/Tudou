/**
 * @file main.cpp
 * @brief FileLinkServer 示例程序（上传文件 -> 返回 URL；访问 URL -> 下载文件）
 */

#include <iostream>
#include <string>
#include <vector>

#include "ConfigLoader.h"
#include "FileLinkServer.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

static void set_logger(const std::string& logPath) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true));

    auto logger = std::make_shared<spdlog::logger>("filelink", sinks.begin(), sinks.end());
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    spdlog::set_level(spdlog::level::err); // debug、info、warn、err、critical
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] [thread %t] %v");
}

int main(int argc, char* argv[]) {
    FileLinkServerBootstrap bootstrap;
    std::string error;
    if (!load_filelink_server_bootstrap(argc, argv, bootstrap, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    set_logger(bootstrap.logPath);
    FileLinkServerConfig cfg = std::move(bootstrap.cfg);

    if (cfg.authEnabled && (cfg.authUser.empty() || cfg.authPassword.empty())) {
        spdlog::warn("auth.enabled=true but auth.user/auth.password not set; all logins will fail.");
    }

    FileLinkServer server(std::move(cfg));

    std::cout << "FileLinkServer started: http://" << cfg.ip << ":" << cfg.port << std::endl;
    std::cout << "Homepage:  GET  /" << std::endl;
    std::cout << "Upload:    POST /upload (Header: X-File-Name)" << std::endl;
    std::cout << "Download:  GET  /file/{id}" << std::endl;

    server.start();
    return 0;
}
