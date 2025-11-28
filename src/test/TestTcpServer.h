/**
 * @file TestServer.h
 * @brief 单元测试。结合底层网络库，测试上层 TcpServer 功能。
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou.git
 */

#include <iostream>
#include <memory>
#include <string>

class EventLoop;
class InetAddress;
class TcpServer;

class TestTcpServer {
public:
    TestTcpServer(int _port = 8080, const std::string& _responseFilepath = "/home/wxm/Tudou/assets/homepage.html");
    ~TestTcpServer();

    void start();

private:
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<InetAddress> listenAddr;
    std::unique_ptr<TcpServer> server;

    int port;
    std::string responseFilepath;
};