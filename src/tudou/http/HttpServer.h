/**
 * @file HttpServer.h
 * @brief HTTP 服务器类，基于 Tudou TcpServer 实现 HTTP 协议封装
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 *
 * HttpServer 对 TcpServer 进行 HTTP 语义封装：
 *  - 每个连接 fd 维护一个 HttpContext，负责 HTTP 报文解析
 *  - 收到完整的 HttpRequest 后，交给上层回调生成 HttpResponse
 *  - 将 HttpResponse 打包为字符串并通过 TcpServer 发送
 *  - 对上层暴露的是 HTTP 语义，而不是底层 TcpConnection
 */

#pragma once
#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <string>

#include "tudou/tcp/TcpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/HttpContext.h"

class HttpServer {
    // 上层业务回调：根据 HttpRequest 填充 HttpResponse
    using HttpCallback = std::function<void(const HttpRequest& req, HttpResponse& resp)>;

private:
    std::string ip;
    uint16_t port;

    int threadNum;
    std::unique_ptr<TcpServer> tcpServer;

    std::unordered_map<int, std::shared_ptr<HttpContext>> httpContexts; // 以连接 fd 作为 key 维护每个连接的 HttpContext。使用 shared_ptr 以便在缩小锁粒度后，仍能安全在锁外使用 HttpContext
    std::mutex contextsMutex;

    HttpCallback httpCallback;

public:
    HttpServer(std::string _ip, uint16_t _port, int _threadNum = 0);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer() = default;

    void set_http_callback(const HttpCallback& cb) { httpCallback = cb; } // 设置上层 HTTP 业务回调

    void start(); 

private:
    void on_connect(const std::shared_ptr<TcpConnection>& conn);
    void on_message(const std::shared_ptr<TcpConnection>& conn);
    void on_close(const std::shared_ptr<TcpConnection>& conn);

    /*
        on_message 的辅助函数。按理说我们的整个流程分为五步：
            1. 接收数据：通过 conn->receive() 获取数据
            2. 解析数据（解析接收到的数据，解析结果放到对应的 HttpContext 中；若完整报文就进入后续步骤）
            3. 业务逻辑处理（调用业务回调 httpCallback 处理请求并封装响应）
            4. 构造响应报文（将 HttpResponse 打包为字符串）
            5. 发送响应（通过 conn->send() 发送响应字符串）
    */
    std::string receive_data(const std::shared_ptr<TcpConnection>& conn);
    void parse_receive_data(const std::shared_ptr<TcpConnection>& conn, const std::string& data);
    void process_data(const std::shared_ptr<TcpConnection>& conn, HttpContext& ctx);
    std::string package_response_data(const HttpResponse& resp);
    void send_data(const std::shared_ptr<TcpConnection>& conn, const std::string& response);

    void handle_http_callback(const HttpRequest& req, HttpResponse& resp); // 实际调用上层 httpCallback 的封装函数

    void check_and_set_content_length(HttpResponse& resp); // 检查并设置 Content-Length 头

    HttpResponse generate_bad_response(); // 生成 400 Bad Request Http 响应报文
    HttpResponse generate_404_response(); // 生成 404 Not Found Http 响应报文

};
