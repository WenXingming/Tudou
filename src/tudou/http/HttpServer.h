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
public:
    // 数据流向：Tcp --> HttpRequest --> 上层业务回调 httpCallback 并填充 HttpResponse --> HttpResponse --> Tcp
    using HttpCallback = std::function<void(const HttpRequest& req, HttpResponse& resp)>;

    // ==================== 构造与析构 ====================
    HttpServer(std::string _ip, uint16_t _port, int _threadNum = 0);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer() = default;

    // ==================== 公共接口 ====================
    // 启动 HTTP 服务器
    void start();

    // 设置 HTTP 请求处理回调
    void set_http_callback(const HttpCallback& cb);

    // 获取服务器配置信息
    const std::string& get_ip() const;
    int get_port() const;
    int get_thread_num() const;

private:
    // ==================== TcpServer 回调处理 ====================
    // TcpServer 事件回调：连接建立、消息到达、连接关闭
    void on_connect(const std::shared_ptr<TcpConnection>& conn);
    void on_message(const std::shared_ptr<TcpConnection>& conn);
    void on_close(const std::shared_ptr<TcpConnection>& conn);

    // ==================== HTTP 请求处理流程（5 个步骤）====================
    /*
     * HTTP 请求处理的完整流程分为以下五个步骤：
     *   1. receive_data         - 接收数据：通过 conn->receive() 从 TcpConnection 获取原始数据
     *   2. parse_receive_data   - 解析数据：将原始数据解析为 HTTP 请求，存入 HttpContext
     *   3. process_data         - 业务处理：调用业务回调处理完整请求并生成响应
     *   4. package_response_data- 打包响应：将 HttpResponse 对象序列化为字符串
     *   5. send_data            - 发送响应：通过 conn->send() 发送响应数据
     */
    std::string receive_data(const std::shared_ptr<TcpConnection>& conn);
    void parse_receive_data(const std::shared_ptr<TcpConnection>& conn, const std::string& data);
    void process_data(const std::shared_ptr<TcpConnection>& conn, HttpContext& ctx);
    std::string package_response_data(const HttpResponse& resp);
    void send_data(const std::shared_ptr<TcpConnection>& conn, const std::string& response);

    // ==================== HTTP 业务逻辑辅助 ====================
    // 调用上层业务回调处理 HTTP 请求
    void handle_http_callback(const HttpRequest& req, HttpResponse& resp);
    // 检查并自动设置 Content-Length 响应头
    void check_and_set_content_length(HttpResponse& resp);

    // ==================== HTTP 默认响应生成 ====================
    // 生成 400 Bad Request 响应
    HttpResponse generate_bad_response();
    // 生成 404 Not Found 响应
    HttpResponse generate_404_response();

    // ==================== 成员变量 ====================
    // 服务器配置
    std::string ip;                              // 监听 IP 地址
    uint16_t port;                               // 监听端口
    std::unique_ptr<TcpServer> tcpServer;        // 底层 TCP 服务器

    // HTTP 上下文管理（每个连接维护一个 HttpContext 用于解析 HTTP 报文）
    std::unordered_map<int, std::shared_ptr<HttpContext>> httpContexts;  // fd -> HttpContext 映射
    std::mutex contextsMutex;                                              // 保护 httpContexts 的互斥锁

    // 业务回调
    HttpCallback httpCallback;                   // 上层 HTTP 请求处理回调
};
