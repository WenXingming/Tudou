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
    : ip(std::move(ip))
    , port(port)
    , responseFilepath(responseFilepath) {

    int ioLoopNum = 0; // IO 线程数量，0 表示不启用 IO 线程池，所有连接都在主线程（监听线程）处理
    tcpServer.reset(new TcpServer(this->ip, this->port, ioLoopNum));
    tcpServer->set_connection_callback(
        [this](int fd) {
            on_connect(fd);
        }
    );
    tcpServer->set_message_callback(
        [this](int fd, const std::string& msg) {
            on_message(fd, msg);
        }
    );
    tcpServer->set_close_callback(
        [this](int fd) {
            on_close(fd);
        }
    );

}

SendFileTcpServer::~SendFileTcpServer() {}

void SendFileTcpServer::start() {
    tcpServer->start();
}

// 没有做任何处理，仅打印日志。使用到 HttpServer 时可能需要设置真正的回调逻辑（从 HttpServer 中构造 HttpContext 等）
void SendFileTcpServer::on_connect(int fd) {
    spdlog::info("New connection established. fd: {}", fd);
}

void SendFileTcpServer::on_message(int fd, const std::string& msg) {
    /*
        按理说我们的整个流程分为五步：
            1. 接收数据
            2. 解析数据
            3. 业务逻辑处理
            4. 构造响应报文
            5. 发送响应
        之前的设计中, on_message 函数的参数是 TcpConnection 对象指针，我们可以直接通过该对象接收数据并发送响应。但是现在 on_message 函数的参数变成了 fd 和 msg，这样设计的好处是降低了类之间的耦合，让上层业务逻辑不需要直接依赖 TcpConnection 类，从而提高了代码的灵活性和可维护性。因此第 1 步和第 5 步需要通过 TcpServer 提供的接口来完成，而不是直接通过 TcpConnection 对象。
    */

    // 1. 接收数据
    std::string data(msg);
    // 2. 解析数据
    std::string request = parse_data(data);
    // 3. 业务逻辑处理
    std::string body = process_business(request);
    // 4. 构造响应报文
    std::string response = construct_response(body);
    // 5. 发送响应
    send_response(fd, response);
}

// 没有做任何处理，仅打印日志。使用到 HttpServer 时可能需要设置真正的回调逻辑（清理 HttpContext 等）
void SendFileTcpServer::on_close(int fd) {
    spdlog::info("Connection closed. fd: {}", fd);
}

// 2. 解析数据（简单起见，Tcp 层可以不做任何解析。Http 的话就需要进行解析）
std::string SendFileTcpServer::parse_data(const std::string& data) {
    std::string response(data);
    return std::move(response);
}

// 3. 业务逻辑处理。应该根据解析得到的 request 内容进行相应的业务逻辑处理，返回业务逻辑处理后，要发送的响应体内容。下面这两个简单示例都没有根据 request 内容进行处理，实际应用中应该根据 request 内容来决定如何处理：
//  - 简单业务逻辑就是直接 echo 回去：conn->send(msg);
//  - 或者返回一个固定的 html 页面
std::string SendFileTcpServer::process_business(const std::string& request) {
    // return std::string("Hello, world");

    std::string filepath = responseFilepath;
    std::ifstream file(filepath, std::ios::binary);

    std::string fileContent;
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        fileContent = buffer.str();
    }
    else {
        spdlog::error("File not found: {}", filepath);
        return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    }

    return fileContent;
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
void SendFileTcpServer::send_response(int fd, const std::string& response) {
    tcpServer->send_message(fd, response);
}