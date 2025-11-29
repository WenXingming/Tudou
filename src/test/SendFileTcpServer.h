/**
 * @file TestServer.h
 * @brief 单元测试。结合底层网络库，测试上层 TcpServer 功能。
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou.git
 */

#pragma once
#include <iostream>
#include <memory>
#include <string>

class EventLoop;
class InetAddress;
class TcpServer;
class TcpConnection;

class SendFileTcpServer {
public:
    SendFileTcpServer();
    ~SendFileTcpServer();

    std::string get_ip() const { return ip; }
    void set_ip(const std::string& _ip) { ip = _ip; }
    uint16_t get_port() const { return port; }
    void set_port(uint16_t _port) { port = _port; }
    std::string get_response_filepath() const { return responseFilepath; }
    void set_response_filepath(const std::string& filepath) {
        responseFilepath = filepath;
    }

    void start();

private:
    void connect_callback(const std::shared_ptr<TcpConnection>& conn);
    void message_callback(const std::shared_ptr<TcpConnection>& conn);

    std::string receive_data(const std::shared_ptr<TcpConnection>& conn);
    std::string parse_data(const std::string& data);
    std::string process_business(const std::string& request);
    std::string construct_response(const std::string& body);
    void send_response(const std::shared_ptr<TcpConnection>& conn, const std::string& response);

private:
    std::string ip{ "127.0.0.1" };
    uint16_t port{ 8080 };
    std::string responseFilepath{ "/home/wxm/Tudou/assets/homepage.html" };

    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<InetAddress> listenAddr;
    std::unique_ptr<TcpServer> tcpServer;
};