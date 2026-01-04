/**
 * @file main.cpp
 * @brief server 示例程序
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include <cassert>
#include <thread>
#include <unistd.h>
#include <string>

#include "StaticFileTcpServer.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h" // support for basic file logging
#include "spdlog/sinks/stdout_color_sinks.h" // support for colored console logging

// ======================================================================================
// 设置并注册 logger
void set_logger() {
    // 创建 sinks 列表
    std::vector<spdlog::sink_ptr> sinks;

    // 添加控制台 sink（带颜色）
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    // 添加文件 sink（覆盖模式）
    // sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/tudou_test.log", true));
    // 添加文件 sink（追加模式）
    // sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("/home/wxm/Tudou/logs/tudou_test.log", false));
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("/home/wxm/Tudou/logs/tudou_test.log", true));

    // 创建组合 logger
    auto my_logger = std::make_shared<spdlog::logger>("multi_sink_logger", begin(sinks), end(sinks));

    // 注册并设为默认 logger
    spdlog::register_logger(my_logger);
    spdlog::set_default_logger(my_logger);

    // 设置日志级别和格式: debug、info、warn、err、critical 等
    spdlog::set_level(spdlog::level::err); // 设置全局日志级别
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] [thread %t] %v"); // 设置日志格式
}

// 测试 logger 功能
void test_logger() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "test_logger running...\n";

    spdlog::debug("This is a debug message.");
    spdlog::info("This is an info message.");
    spdlog::warn("This is a warning message.");
    spdlog::error("This is an error message.");
    spdlog::critical("This is a critical message.");

    std::cout << "test_logger success.\n";
    std::cout << "==================================================================" << std::endl;
}


void run_static_tcp_server() {
    std::string ip = "192.168.3.3";
    int port = 8080;
    std::string filepath = "/home/wxm/Tudou/assets/hello-world.html";
    int threadNum = 16; // 线程数量（设置为 N 则多 Reactor；设置为 0 则单 Reactor）

    std::cout << "Starting TcpServer test on " << ip << ":" << port << "..." << std::endl;
    std::thread t2([ip, port, filepath, threadNum]() {
        StaticFileTcpServer sendFileTcpServer(ip, port, filepath, threadNum);
        sendFileTcpServer.start();
        });
    t2.join();
    std::cout << "TcpServer test finished." << std::endl;
}

int main() {
    set_logger();
    // test_logger();
    
    run_static_tcp_server();

    return 0;
}
