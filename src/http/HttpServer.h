/**
 * @file HttpServer.h
 * @brief HTTP 服务器类，基于 Tudou TcpServer 实现 HTTP 协议封装
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#pragma once
#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <string>

#include "../tudou/TcpServer.h"
#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"
#include "../http/HttpContext.h"

// HttpServer 对 TcpServer 进行 HTTP 语义封装：
//  - 每个连接 fd 维护一个 HttpContext，负责 HTTP 报文解析
//  - 收到完整的 HttpRequest 后，交给上层回调生成 HttpResponse
//  - 将 HttpResponse 打包为字符串并通过 TcpServer 发送
//  - 对上层暴露的是 HTTP 语义，而不是底层 TcpConnection
class HttpServer {
    // 上层业务回调：根据 HttpRequest 填充 HttpResponse
    using HttpCallback = std::function<void(const HttpRequest& req, HttpResponse& resp)>;

private:
    std::string ip;
    uint16_t port;
    std::unique_ptr<TcpServer> tcpServer;
    int threadNum;

    // 以连接 fd 作为 key 维护每个连接的 HttpContext
    // 使用 shared_ptr 以便在缩小锁粒度后，仍能安全在锁外使用 HttpContext
    std::unordered_map<int, std::shared_ptr<HttpContext>> httpContexts;
    std::mutex contextsMutex;

    HttpCallback httpCallback;

public:
    HttpServer(std::string _ip, uint16_t _port, int _threadNum = 0);
    ~HttpServer() = default;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 设置上层 HTTP 业务回调
    void set_http_callback(const HttpCallback& cb) { httpCallback = cb; }

    void start();

private:
    void on_connect(int fd);

    // TcpServer 的消息回调入口：fd + 原始数据
    void on_message(int fd, const std::string& msg);
    /*
        on_message 的辅助函数。按理说我们的整个流程分为五步：
            1. 接收数据
            2. 解析数据
            3. 业务逻辑处理（调用 messageCallback 由上层进行业务逻辑处理）
            4. 构造响应报文
            5. 发送响应
        之前的设计中, on_message 函数的参数是 TcpConnection 对象指针，我们可以直接通过该对象接收数据并发送响应。
        但是现在 on_message 函数的参数变成了 fd 和 msg，这样设计的好处是降低了类之间的耦合，让上层业务逻辑不需要直接依赖 TcpConnection 类，从而提高了代码的灵活性和可维护性。
        因此第 1 步和第 5 步需要通过 TcpServer 提供的接口来完成，而不是直接通过 TcpConnection 对象。
    */
    // 第 1 步：接收数据（目前直接返回 msg，本函数主要为流程语义保留）
    std::string receive_data(const std::string& data);
    // 第 2 步：解析接收到的数据，解析结果放到对应的 HttpContext 中；若完整报文就进入后续步骤
    void parse_receive_data(int fd, const std::string& data);
    // 第 3~4 步：调用业务回调处理请求并封装响应
    void process_data(int fd, HttpContext& ctx);
    // 第 4 步辅助：将 HttpResponse 打包为字符串
    std::string package_response_data(const HttpResponse& resp);
    // 第 5 步：通过 TcpServer 发送响应字符串
    void send_data(int fd, const std::string& response);

    void on_close(int fd);

    // 实际调用上层 httpCallback 的封装
    void handle_http_callback(const HttpRequest& req, HttpResponse& resp);

    std::string generate_bad_response();
    std::string generate_404_response();

    
};
