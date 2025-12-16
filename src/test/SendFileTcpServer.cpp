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

SendFileTcpServer::SendFileTcpServer(std::string _ip, uint16_t _port, const std::string& _responseFilepath, const int _threadNum) :
    ip(std::move(_ip)),
    port(_port),
    responseFilepath(_responseFilepath),
    tcpServer(nullptr),
    threadNum(_threadNum) {
        
    int ioLoopNum = threadNum; // IO 线程数量，0 表示不启用 IO 线程池，所有连接都在主线程（监听线程）处理
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
    spdlog::info("SendFileTcpServer::on_connect(): New connection established. fd: {}", fd);
}

void SendFileTcpServer::on_message(int fd, const std::string& msg) {
    // 1. 接收数据
    std::string data = receive_data(msg);
    // 2. 解析数据
    std::string request = parse_receive_data(data);
    // 3. 业务逻辑处理
    std::string body = process_data(request);
    // 4. 构造响应报文
    std::string response = package_response_data(body);
    // 5. 发送响应
    send_data(fd, response);
}

// 没有做任何处理，仅打印日志。使用到 HttpServer 时可能需要设置真正的回调逻辑（清理 HttpContext 等）
void SendFileTcpServer::on_close(int fd) {
    spdlog::info("Connection closed. fd: {}", fd);
}

std::string SendFileTcpServer::receive_data(const std::string& _data) {
    // 1. 接收数据。这里直接返回传入的数据，因为 on_message 已经接收到了数据（经过我们修改的 TcpServer 类，使其在回调中直接传递接收到的数据，不再传递 TcpConnection 对象）
    return _data; // 拷贝 + 隐式移动优化
}

std::string SendFileTcpServer::parse_receive_data(const std::string& data) {
    // 2. 解析数据（简单起见，该应用服务基于 Tcp 层可以不做任何解析。Http 的话就需要进行解析）
    std::string response(data); // 显式拷贝
    return std::move(response); // 显式移动
}

std::string SendFileTcpServer::process_data(const std::string& request) {
    // 3. 业务逻辑处理。应该根据解析得到的 request 内容进行相应的业务逻辑处理，返回业务逻辑处理后，要发送的响应体内容。下面这两个简单示例都没有根据 request 内容进行处理，实际应用中应该根据 request 内容来决定如何处理：
    //  - 简单业务逻辑就是直接 echo 回去：conn->send(msg);
    //  - 或者返回一个固定的 html 页面内容：
    
    return std::string("Hello, world"); // 测试用，不从硬盘读取文件，避免 IO 操作影响性能测试结果。不过测试结果看起来速度还是差不多

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
std::string SendFileTcpServer::package_response_data(const std::string& body) {
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
void SendFileTcpServer::send_data(int fd, const std::string& response) {
    tcpServer->send_message(fd, response);
}