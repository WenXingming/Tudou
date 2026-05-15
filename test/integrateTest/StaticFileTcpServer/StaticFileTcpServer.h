/**
 * @file StaticFileTcpServer.h
 * @brief 发送文件的 TCP 服务器示例
 * @details 得益于 Tudou 框架的模块化设计，实现一个发送文件的 TCP 服务器变得非常简单。只需持有 Tudou 提供的 TcpServer 类，并设置相应的回调函数即可完成文件发送功能。TcpServer 的回调函数如下：
 * - 连接建立回调: on_connect(conn)
 * - 消息接收回调: on_message(conn, const std::string& msg)。 在消息接收回调中，我们可以进行业务逻辑编写，例如读取指定文件的内容，并将其作为响应发送回客户端。
 * - 连接关闭回调: on_close(conn)
 *
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once
#include <iostream>
#include <memory>
#include <string>
#include <cstdint>

#include "tudou/tcp/TcpServer.h"

class EventLoop;

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
    void on_connect(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn, const std::string& data);
    void on_close(const TcpConnectionPtr& conn);

    /*
        on_message 的辅助函数。按理说我们的整个流程分为五步：
            1. 接收数据：TcpServer 在回调前从 TcpConnection 的 readBuffer 中读取数据
            2. 解析数据
            3. 业务逻辑处理
            4. 构造响应报文
            5. 发送响应：通过当前 TcpConnection 发送数据
     */
    std::string receive_data(const std::string& data);
    std::string parse_received_data(const std::string& data);
    std::string process_data(const std::string& request);
    std::string package_response_data(const std::string& body);
    void send_data(const TcpConnectionPtr& conn, const std::string& response);
};
