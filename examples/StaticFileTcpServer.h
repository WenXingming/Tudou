/**
 * @file StaticFileTcpServer.h
 * @brief 发送文件的 TCP 服务器示例
 * @details 得益于 Tudou 框架的模块化设计，实现一个发送文件的 TCP 服务器变得非常简单。只需持有 Tudou 提供的 TcpServer 类，并设置相应的回调函数即可完成文件发送功能。TcpServer 的回调函数如下：
 * - 连接建立回调: on_connect(int fd)
 * - 消息接收回调: on_message(int fd, const std::string& msg)。 在消息接收回调中，我们可以进行业务逻辑编写，例如读取指定文件的内容，并将其作为响应发送回客户端。
 * - 连接关闭回调: on_close(int fd)
 *
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once
#include <iostream>
#include <memory>
#include <string>

class EventLoop;
class TcpServer;
class TcpConnection;

class StaticFileTcpServer {
private:
    std::string ip;
    uint16_t port;
    std::string responseFilepath;

    std::unique_ptr<TcpServer> tcpServer;
    int threadNum;

public:
    StaticFileTcpServer(std::string _ip, uint16_t _port, const std::string& _responseFilepath, const int _threadNum = 12);
    ~StaticFileTcpServer();

    std::string get_ip() const { return ip; }
    uint16_t get_port() const { return port; }
    std::string get_response_filepath() const { return responseFilepath; }
    void set_response_filepath(const std::string& filepath) { responseFilepath = filepath; }

    void start();

private:
    void on_connect(int fd);
    void on_message(int fd, const std::string& msg);
    void on_close(int fd);

    /*
        on_message 的辅助函数。按理说我们的整个流程分为五步：
            1. 接收数据
            2. 解析数据
            3. 业务逻辑处理
            4. 构造响应报文
            5. 发送响应
        之前的设计中, on_message 函数的参数是 TcpConnection 对象指针，我们可以直接通过该对象接收数据并发送响应。
        但是现在 on_message 函数的参数变成了 fd 和 msg，这样设计的好处是降低了类之间的耦合，让上层业务逻辑不需要直接依赖 TcpConnection 类，从而提高了代码的灵活性和可维护性。
        因此第 1 步和第 5 步需要通过 TcpServer 提供的接口来完成，而不是直接通过 TcpConnection 对象。
    */
    std::string receive_data(const std::string& data);
    std::string parse_receive_data(const std::string& data);
    std::string process_data(const std::string& request);
    std::string package_response_data(const std::string& body);
    void send_data(int fd, const std::string& response);
};