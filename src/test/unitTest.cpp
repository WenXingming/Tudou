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

#include "TestNetlib.h"
#include "SendFileTcpServer.h"
#include "TestHttpParser.h"
#include "TestHttpServer.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h" // support for basic file logging
#include "spdlog/sinks/stdout_color_sinks.h" // support for colored console logging

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

    // 设置日志级别和格式
    spdlog::set_level(spdlog::level::debug);
    // spdlog::set_level(spdlog::level::info);
    // spdlog::set_level(spdlog::level::err);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] [thread %t] %v");
}


int main() {
    set_logger();

    // 测试网络库：EventLoop、Epoller、Channel
    std::thread t1([]() {
        TestNetlib testNetlib;
        testNetlib.start();
        });
    t1.join();
    std::cout << "Netlib test finished." << std::endl;

    // 测试 TcpServer 服务器：网络库 + TcpServer、Acceptor、TcpConnection、Buffer
    // std::thread t2([]() {
    //     SendFileTcpServer sendFileTcpServer("127.0.0.1", 8080, "/home/wxm/Tudou/assets/happy-birthday.html");
    //     sendFileTcpServer.start();
    //     });
    // t2.join();
    // std::cout << "TcpServer test finished." << std::endl;

    // 测试 HTTP 报文解析器
    // 命令行测试： curl -v http://127.0.0.1:8080/ -o /dev/null
    // std::thread t3([]() {
    //     TestHttpParser testHttpParser;
    //     int ret = testHttpParser.run_all();
    //     assert(ret == 0);
    //     });
    // t3.join();
    // std::cout << "HttpParser test finished." << std::endl;


    // // 测试 HttpServer 服务器：TcpServer + HttpServer
    // spdlog::info("Starting HttpServer test...");
    // std::thread t4([]() {
    //     TestHttpServer testHttpServer(8080, "/home/wxm/Tudou/assets/homepage.html");
    //     // TestHttpServer testHttpServer(8080, "/home/wxm/Tudou/assets/hello-world.html");
    //     testHttpServer.start();
    //     });
    // t4.join();
    // std::cout << "HttpServer test finished." << std::endl;
    return 0;
}
