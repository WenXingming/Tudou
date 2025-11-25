/**
 * @file main.cpp
 * @brief 单元测试
 * @author wenxingming
 * @date 2025-09-06
 * @project: https://github.com/WenXingming/Multi_IO
 */

#include <iostream>
#include <unistd.h>
#include <cassert>
#include <thread>

#include "../base/Timestamp.h"
#include "../base/Log.h"
#include "../base/InetAddress.h"
#include "Channel.h"
#include "Buffer.h"
#include "TcpConnection.h"

#include "EventLoop.h"
#include "TcpServer.h"

 /**
  * @brief 直接使用网络库功能进行测试。还没有封装上层 TcpServer
  */
void test_netlib() {
    EventLoop loop;

    Channel stdinChannel(&loop, 0, 0, 0, nullptr, nullptr, nullptr, nullptr); // fd = 0 (标准输入)
    stdinChannel.enable_reading();
    stdinChannel.subscribe_on_read([&](/* Timestamp receivetime */) {
        char buf[1024]{};
        ssize_t n = read(0, buf, sizeof(buf) - 1);
        if (n > 0) {
            std::cout << "stdin: " << buf << std::endl;
        }
        });
    loop.update_channel(&stdinChannel);

    loop.loop();
}


/**
 * @brief 测试上层 TcpServer
 */
void test_server() {
    EventLoop loop;
    InetAddress listenAddr = InetAddress(8080);
    TcpServer server(&loop, listenAddr, nullptr);

    server.subscribe_message(
        [](const std::shared_ptr<TcpConnection>& conn) {
            std::string msg(conn->recv());
            // std::cout << "Received: " << msg << std::endl;
            // 业务处理 ...
            // http 解析
            char writeBuffer[1024] = {};
            int writeLength = 1024;
            writeLength = sprintf(writeBuffer,
                "HTTP/1.1 200 OK\r\n"
                "Accept-Ranges: bytes\r\n"
                "Content-Length: 90\r\n"
                "Content-Type: text/html\r\n"
                "Date: Sat, 06 Aug 2022 13:16:46 GMT\r\n\r\n"
                "<html><head><title>0voice.king</title></head><body><h1>Hello, World</h1></body></html>\r\n\r\n");
            // echo 回去
            // conn->send(msg);
            conn->send(std::string(writeBuffer));
        }
    );

    server.start();
    loop.loop();
}

int main() {
    // 日志系统
    // LOG::disable_debug();

    // std::thread t1(test_netlib);
    // t1.join();

    std::thread t2(test_server);
    t2.join();

    return 0;
}
