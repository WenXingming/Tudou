/**
 * @file SendFileTcpServer.h
 * @brief 发送文件的 TCP 服务器示例
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
#include "../base/InetAddress.h" // 需要包含完整定义，因为成员变量中有 InetAddress 对象（不是指针或引用）

class SendFileTcpServer {
public:
    SendFileTcpServer(std::string ip, uint16_t port, const std::string& responseFilepath);
    ~SendFileTcpServer();

    std::string get_ip() const { return ip; }
    // void set_ip(const std::string& _ip) { ip = _ip; }
    uint16_t get_port() const { return port; }
    // void set_port(uint16_t _port) { port = _port; }
    std::string get_response_filepath() const { return responseFilepath; }
    void set_response_filepath(const std::string& filepath) {
        responseFilepath = filepath;
    }

    void start();

private:
    void connect_callback(int fd);
    void message_callback(int fd, const std::string& msg);
    void close_callback(int fd);

    // std::string receive_data(int fd);
    std::string parse_data(const std::string& data);
    std::string process_business(const std::string& request);
    std::string construct_response(const std::string& body);
    void send_response(int fd, const std::string& response);

private:
    std::string ip{ "127.0.0.1" };
    uint16_t port{ 8080 };
    std::string responseFilepath{ "/home/wxm/Tudou/assets/homepage.html" };

    InetAddress listenAddr;
    std::unique_ptr<TcpServer> tcpServer;
};