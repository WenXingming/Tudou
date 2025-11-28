/**
 * @file main.cpp
 * @brief 单元测试
 * @author wenxingming
 * @date 2025-09-06
 * @project: https://github.com/WenXingming/Tudou.git
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <cassert>
#include <thread>

#include "../base/Timestamp.h"
#include "../base/Log.h"

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

    // 测试 TcpServer 服务器：网络库 + TcpServer
    // std::thread t2([]() {
    //     TestTcpServer testTcpServer(8080, "/home/wxm/Tudou/assets/homepage.html");
    //     testTcpServer.start();
    //     });
    // t2.join();

    // 测试 HTTP 报文解析器
    // std::thread t3([]() {
    //     tudou::TestHttpParser testHttpParser;
    //     int ret = testHttpParser.run_all();
    //     assert(ret == 0);
    //     });
    // t3.join();

    // 测试 HttpServer 服务器：TcpServer + HttpServer
    std::thread t4([]() {
        tudou::TestHttpServer testHttpServer(8080, "/home/wxm/Tudou/assets/homepage.html");
        testHttpServer.start();
        });
    t4.join();
    return 0;
}
