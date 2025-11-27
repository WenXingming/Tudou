#include "../base/InetAddress.h"
#include "../tudou/TcpServer.h"
#include "../tudou/EventLoop.h"
#include "../tudou/TcpConnection.h"
#include "../tudou/Buffer.h"
#include "TestServer.h"

#include <iostream>
#include <fstream>
#include <sstream>

TestServer::TestServer(int _port, const std::string& _responseFilepath) : port(_port), responseFilepath(_responseFilepath) {
    // loop = std::make_unique<EventLoop>(); # c++14 还是 17 支持
    loop.reset(new EventLoop());
    listenAddr.reset(new InetAddress(port /* port */, "127.0.0.1"));
    server.reset(new TcpServer(
        loop.get(),
        *listenAddr,
        nullptr /* 先不设置回调，在下面单独设置 */
    ));

    server->set_message_callback(
        [this](const std::shared_ptr<TcpConnection>& conn) {
            // 1. 接收数据
            std::string msg(conn->receive());
            std::cout << "Received: " << msg << std::endl;

            // 2. http 解析 + 业务逻辑处理
            //  - 简单业务逻辑就是不解析 http 报文，直接 echo 回去：conn->send(msg);
            //  - 或者返回一个固定的 html 页面

            // 3. 组织响应数据发送
            // 简单的 http 响应报文
            // char writeBuffer[1024] = {};
            // int writeLength = 1024;
            // writeLength = sprintf(writeBuffer,
            //     "HTTP/1.1 200 OK\r\n"
            //     "Accept-Ranges: bytes\r\n"
            //     "Content-Length: 90\r\n"
            //     "Content-Type: text/html\r\n"
            //     "Date: Sat, 06 Aug 2022 13:16:46 GMT\r\n\r\n"
            //     "<html><head><title>0voice.king</title></head><body><h1>Hello, World</h1></body></html>\r\n\r\n");
            // conn->send(std::string(writeBuffer/* , writeLength */));

            // 发送文件 ../assets/homepage.html
            //  1. 打开文件 (这里硬编码为 index.html 用于测试，请确保运行目录下有此文件)。
            // 没有发送内容这通常是因为相对路径的问题。C++ 程序中的相对路径是相对于执行该程序时的当前工作目录，而不是相对于源代码文件所在的目录。也可以使用绝对路径，例如: "/home/wxm/Tudou/assets/index.html"
            // std::string filepath = "../assets/homepage.html";
            std::string filepath = responseFilepath;
            std::ifstream file(filepath, std::ios::binary);

            if (file.is_open()) {
                // 2. 读取文件所有内容到 string
                std::stringstream buffer;
                buffer << file.rdbuf();
                std::string fileContent = buffer.str();

                // 3. 构建 HTTP 响应
                std::string response;
                response.reserve(fileContent.size() + 256); // 预分配内存避免多次拷贝

                response += "HTTP/1.1 200 OK\r\n";
                response += "Content-Type: text/html; charset=utf-8\r\n"; // 根据文件类型修改
                response += "Content-Length: " + std::to_string(fileContent.size()) + "\r\n";
                response += "Connection: Keep-Alive\r\n";
                response += "\r\n"; // 空行分割 Header 和 Body

                response += fileContent; // 拼接文件内容

                // 4. 发送
                conn->send(response);
            }
            else {
                // 5. 文件不存在，返回 404
                std::cout << "File not found: " << filepath << std::endl; // 调试输出文件不存在信息
                std::string response =
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Length: 0\r\n\r\n";
                conn->send(response);
            }
        }
    );
}

TestServer::~TestServer() {}

void TestServer::start() {
    server->start();
    loop->loop();
}