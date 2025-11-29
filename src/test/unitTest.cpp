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
#include "TestTcpServer.h"
#include "TestHttpParser.h"
#include "TestHttpServer.h"

int main() {
    // 日志系统
    // LOG::disable_debug();

    // 测试网络库：EventLoop、Epoller、Channel
    // std::thread t1([]() {
    //     TestNetlib testNetlib;
    //     testNetlib.start();
    //     });
    // t1.join();
    // std::cout << "Netlib test finished." << std::endl;

    // 测试 TcpServer 服务器：网络库 + TcpServer、Acceptor、TcpConnection、Buffer
    // std::thread t2([]() {
    //     TestTcpServer testTcpServer(8080, "/home/wxm/Tudou/assets/homepage.html");
    //     testTcpServer.start(); });
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
