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

int main() {
    spdlog::set_level(spdlog::level::debug); // 设置全局日志级别为 debug。可以显示 debug 及以上级别日志（info、warn、err 等）

    // 测试网络库：EventLoop、Epoller、Channel
    // std::thread t1([]() {
    //     TestNetlib testNetlib;
    //     testNetlib.start();
    //     });
    // t1.join();
    // std::cout << "Netlib test finished." << std::endl;

    // 测试 TcpServer 服务器：网络库 + TcpServer、Acceptor、TcpConnection、Buffer
    // std::thread t2([]() {
    //     SendFileTcpServer sendFileTcpServer;
    //     sendFileTcpServer.set_ip("127.0.0.1");
    //     sendFileTcpServer.set_port(8080);
    //     sendFileTcpServer.set_response_filepath("/home/wxm/Tudou/assets/homepage.html");

    //     sendFileTcpServer.start(); }
    // );
    // t2.join();
    // std::cout << "TcpServer test finished." << std::endl;

    // 测试 HTTP 报文解析器
    // 命令行测试： curl -v http://127.0.0.1:8080/ -o /dev/null
    // std::thread t3([]() {
    //     tudou::TestHttpParser testHttpParser;
    //     int ret = testHttpParser.run_all();
    //     assert(ret == 0);
    //     });
    // t3.join();
    // std::cout << "HttpParser test finished." << std::endl;


    spdlog::debug("Starting HttpServer test...");
    // 测试 HttpServer 服务器：TcpServer + HttpServer
    std::thread t4([]() {
        tudou::TestHttpServer testHttpServer(8080, "/home/wxm/Tudou/assets/homepage.html");
        // tudou::TestHttpServer testHttpServer(8080, "/home/wxm/Tudou/assets/hello-world.html");
        testHttpServer.start();
        });
    t4.join();
    std::cout << "HttpServer test finished." << std::endl;
    return 0;
}
