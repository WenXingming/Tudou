/**
 * @file main.cpp
 * @brief 单元测试
 * @author wenxingming
 * @date 2025-09-06
 * @project: https://github.com/WenXingming/Tudou.git
 */

#include <cassert>
#include <thread>
#include <unistd.h>
#include <string>

#include "TestNetlib.h"
#include "StaticFileTcpServer.h"
#include "TestHttpParser.h"
#include "StaticFileHttpServer.h"
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

// ======================================================================================
// 各种测试函数

#include "../base/InetAddress.h"
void test_inet_address() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "test_inet_address running...\n";

    InetAddress inetAddress = InetAddress("127.0.0.1", 8080);
    std::cout << "ip: " << inetAddress.get_ip() << std::endl;
    std::cout << "port: " << inetAddress.get_port() << std::endl;
    std::cout << "ip:port: " << inetAddress.get_ip_port() << std::endl;

    std::cout << "test_inet_address success.\n";
    std::cout << "==================================================================" << std::endl;
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

// 测试网络库：EventLoop、Epoller、Channel
void test_net_library() {
    std::cout << "Starting Netlib test..." << std::endl;
    std::thread t1([]() {
        TestNetlib testNetlib;
        testNetlib.start();
        });
    t1.join();
    std::cout << "Netlib test finished." << std::endl;
}

// 测试 TcpServer 服务器，包含：网络库 + TcpServer、Acceptor、TcpConnection、Buffer 等，即整个网络框架（Tudou）
void test_tcp_server() {
    std::string ip = "127.0.0.1";
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

// 测试 HTTP 报文解析器
void test_http_parser() {
    std::cout << "Starting HttpParser test..." << std::endl;
    // 命令行测试： curl -v http://127.0.0.1:8080/ -o /dev/null
    std::thread t3([]() {
        TestHttpParser testHttpParser;
        int ret = testHttpParser.run_all();
        assert(ret == 0);
        });
    t3.join();
    std::cout << "HttpParser test finished." << std::endl;
}

void test_http_server() {
    // 测试 HttpServer 服务器：TcpServer + HttpServer
    std::cout << "Starting HttpServer test..." << std::endl;

    std::string ip = "127.0.0.1";
    int port = 8080;
    std::string baseDir = "/home/wxm/Tudou/assets";
    int threadNum = 16; // 与 TcpServer 测试保持一致，使用多 Reactor

    std::thread t4([ip, port, baseDir, threadNum]() {
        StaticFileHttpServer server(ip, static_cast<uint16_t>(port), baseDir, threadNum);
        server.start();
        });
    t4.join();
    std::cout << "HttpServer test finished." << std::endl;
}

int main() {
    set_logger();

    test_inet_address();
    test_logger();
    // test_net_library();
    // test_tcp_server();
    // test_http_parser();
    test_http_server();

    return 0;
}
