/**
 * @file SendFileTcpServer.h
 * @brief 发送文件的 TCP 服务器示例
 * @details 得益于 Tudou 框架的模块化设计，实现一个发送文件的 TCP 服务器变得非常简单。只需持有 Tudou 提供的 TcpServer 类，并设置相应的回调函数即可完成文件发送功能。TcpServer 的回调函数如下：
 * - 连接建立回调: on_connect(int fd)
 * - 消息接收回调: on_message(int fd, const std::string& msg)。 在消息接收回调中，我们可以进行业务逻辑编写，例如读取指定文件的内容，并将其作为响应发送回客户端。
 * - 连接关闭回调: on_close(int fd)
 *
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou.git
 */

#pragma once
#include <iostream>
#include <memory>
#include <string>

class EventLoop;
class TcpServer;
class TcpConnection;

class SendFileTcpServer {
private:
    std::string ip{ "127.0.0.1" };
    uint16_t port{ 8080 };
    std::string responseFilepath{ "/home/wxm/Tudou/assets/homepage.html" };

    std::unique_ptr<TcpServer> tcpServer;

public:
    SendFileTcpServer(std::string ip, uint16_t port, const std::string& responseFilepath);
    ~SendFileTcpServer();

    std::string get_ip() const { return ip; }
    uint16_t get_port() const { return port; }
    std::string get_response_filepath() const { return responseFilepath; }
    void set_response_filepath(const std::string& filepath) { responseFilepath = filepath; }

    void start();

private:
    void on_connect(int fd);
    void on_message(int fd, const std::string& msg);
    void on_close(int fd);

    // on_message 的辅助函数
    // std::string receive_data(int fd);
    std::string parse_data(const std::string& data);
    std::string process_business(const std::string& request);
    std::string construct_response(const std::string& body);
    void send_response(int fd, const std::string& response);
};