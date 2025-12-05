#include "SendFileTcpServer.h"

#include "../base/InetAddress.h"
#include "../tudou/TcpServer.h"
#include "../tudou/EventLoop.h"
#include "../tudou/EpollPoller.h"
#include "../tudou/TcpConnection.h"
#include "spdlog/spdlog.h"

#include <iostream>
#include <fstream>
#include <sstream>

SendFileTcpServer::SendFileTcpServer(std::string ip, uint16_t port, const std::string& responseFilepath)
    : listenAddr(ip, port)
    , responseFilepath(responseFilepath) {

    tcpServer.reset(new TcpServer(ip, port));
    tcpServer->set_connection_callback(
        [this](const std::shared_ptr<TcpConnection>& conn) {
            connect_callback(conn);
        }
    );
    tcpServer->set_message_callback(
        [this](const std::shared_ptr<TcpConnection>& conn) {
            message_callback(conn);
        }
    );
    tcpServer->set_close_callback(
        [this](const std::shared_ptr<TcpConnection>& conn) {
            close_callback(conn);
        }
    );

}

SendFileTcpServer::~SendFileTcpServer() {}

void SendFileTcpServer::start() {
    tcpServer->start();
}

// 没有做任何处理，仅打印日志。使用到 HttpServer 时可能需要设置真正的回调逻辑
void SendFileTcpServer::connect_callback(const std::shared_ptr<TcpConnection>& conn) {
    spdlog::info("New connection established. fd: {}", conn->get_fd());
}

void SendFileTcpServer::message_callback(const std::shared_ptr<TcpConnection>& conn) {
    // 1. 接收数据
    std::string data = receive_data(conn);
    // 2. 解析数据
    std::string request = parse_data(data);
    // 3. 业务逻辑处理
    std::string body = process_business(request);
    // 4. 构造响应报文
    std::string response = construct_response(body);
    // 5. 发送响应
    send_response(conn, response);
}

void SendFileTcpServer::close_callback(const std::shared_ptr<TcpConnection>& conn) {
    spdlog::info("Connection closed. fd: {}", conn->get_fd());
}

// 1. 接收数据
std::string SendFileTcpServer::receive_data(const std::shared_ptr<TcpConnection>& conn) {
    std::string msg(conn->receive());
    return std::move(msg);
}

// 2. 解析数据（简单起见，Tcp 层可以不做任何解析）
std::string SendFileTcpServer::parse_data(const std::string& data) {
    std::string response(data);
    return std::move(response);
}

// 3. 业务逻辑处理
//  - 简单业务逻辑就是直接 echo 回去：conn->send(msg);
//  - 或者返回一个固定的 html 页面
std::string SendFileTcpServer::process_business(const std::string& requestData) {
    std::string filepath = responseFilepath;
    std::ifstream file(filepath, std::ios::binary);

    std::string fileContent;
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        fileContent = buffer.str();
    }
    else {
        // 5. 文件不存在，返回 404
        std::cout << "File not found: " << filepath << std::endl; // 调试输出文件不存在信息
    }

    return fileContent;
    // return std::string("Hello, world"); // 返回业务逻辑处理后，要发送的响应体内容
}

// 4. 构造响应报文。根据 body 内容构造完整的响应报文
std::string SendFileTcpServer::construct_response(const std::string& body) {
    std::string response;
    response.reserve(body.size() + 256); // 预分配内存，避免多次拷贝
    response += "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/html; charset=utf-8\r\n"; // 根据
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: Keep-Alive\r\n";
    response += "\r\n"; // 空行分割 Header 和 Body
    response += body;
    return std::move(response);
}

// 5. 发送响应
void SendFileTcpServer::send_response(const std::shared_ptr<TcpConnection>& conn, const std::string& response) {
    conn->send(response);
}