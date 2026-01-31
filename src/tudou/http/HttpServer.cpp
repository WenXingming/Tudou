/**
 * @file HttpServer.cpp
 * @brief HTTP 服务器类，基于 Tudou TcpServer 实现 HTTP 协议封装
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "HttpServer.h"
#include "HttpContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

#include "spdlog/spdlog.h"
#include "tudou/tcp/TcpServer.h"
#include "tudou/tcp/TcpConnection.h"

 // ==================== 构造与析构 ====================

HttpServer::HttpServer(std::string _ip, uint16_t _port, int _threadNum) :
    ip(std::move(_ip)),
    port(_port),
    tcpServer(nullptr),
    httpContexts(),
    contextsMutex(),
    httpCallback(nullptr) {

    // 创建底层 TcpServer
    tcpServer.reset(new TcpServer(this->ip, this->port, _threadNum));

    // 注册 TcpServer 事件回调
    tcpServer->set_connection_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        on_connect(conn);
        });
    tcpServer->set_message_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        on_message(conn);
        });
    tcpServer->set_close_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        on_close(conn);
        });
}

void HttpServer::start() {
    spdlog::debug("HttpServer: Starting HTTP server at {}:{}", ip, port);
    if (!tcpServer) {
        spdlog::critical("HttpServer: tcpServer is nullptr, cannot start server.");
        return;
    }
    tcpServer->start();
}

void HttpServer::set_http_callback(const HttpCallback& cb) {
    httpCallback = cb;
}

const std::string& HttpServer::get_ip() const {
    return ip;
}

int HttpServer::get_port() const {
    return port;
}

int HttpServer::get_thread_num() const {
    return tcpServer ? tcpServer->get_num_threads() : 0;
}

void HttpServer::on_connect(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        if (httpContexts.find(fd) != httpContexts.end()) {
            spdlog::warn("HttpServer: HttpContext already exists for fd={}, overwriting.", fd);
        }
        httpContexts[fd] = std::shared_ptr<HttpContext>(new HttpContext()); // 这里的逻辑是如果存在则覆盖
        spdlog::debug("HttpServer: New connection established, fd={}", fd);
    }
}

void HttpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    std::string receivedData = receive_data(conn); // 启动 HTTP 处理流程。步骤 1：接收原始数据
    parse_received_data(conn, receivedData); // 步骤 2~5：解析 HTTP 请求 -> 业务处理 -> 打包响应 -> 发送
}

void HttpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        if (httpContexts.find(fd) == httpContexts.end()) {
            spdlog::warn("HttpServer: No HttpContext found for fd={} on close.", fd);
            return;
        }
        httpContexts.erase(fd);
        spdlog::debug("HttpServer: Connection closed, fd={}", fd);
    }
}

std::string HttpServer::receive_data(const std::shared_ptr<TcpConnection>& conn) {
    return conn->receive();
}

void HttpServer::parse_received_data(const std::shared_ptr<TcpConnection>& conn, const std::string& receivedData) {
    // 获取该连接对应的 HttpContext（使用 shared_ptr 拷贝缩小锁的临界区，同时保证对象在锁外仍然存活）
    int fd = conn->get_fd();
    std::shared_ptr<HttpContext> ctx;
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        auto it = httpContexts.find(fd);
        if (it == httpContexts.end()) {
            spdlog::error("HttpServer: No HttpContext found for fd={}", fd);
            return;
        }
        ctx = it->second;
    }

    // 解析 HTTP 请求报文
    size_t nparsed = 0;
    bool ok = ctx->parse(receivedData.data(), receivedData.size(), nparsed);

    // 解析失败，返回 400 Bad Request
    if (!ok) {
        spdlog::warn("HttpServer: Failed to parse HTTP request from fd={}", fd);
        HttpResponse resp = generate_bad_response();
        send_data(conn, resp.package_to_string());
        ctx->reset();
        return;
    }

    // 检查 HTTP 请求是否完整。请求未完整接收，等待更多数据（支持长连接场景）
    if (!ctx->is_complete()) {
        spdlog::debug("HttpServer: HTTP request incomplete, waiting for more data, fd={}", fd);
        return;
    }

    // 步骤 3~5：处理完整的 HTTP 请求
    process_data(conn, *ctx);
}

void HttpServer::process_data(const std::shared_ptr<TcpConnection>& conn, HttpContext& ctx) {
    // 获取解析完成的 HTTP 请求
    const HttpRequest& req = ctx.get_request();

    // 创建响应对象并调用业务回调（获取返回的响应内容）
    HttpResponse resp;
    handle_http_callback(req, resp);

    // 自动补充 Content-Length 头（如果业务层未设置）
    check_and_set_content_length(resp);

    // 步骤 4：打包 HTTP 响应
    std::string responseData = package_response_to_string(resp);

    // 步骤 5：发送响应数据
    send_data(conn, responseData);

    // 重置 HttpContext，为处理同一连接的下一个请求做准备（支持长连接）
    ctx.reset();
}

std::string HttpServer::package_response_to_string(const HttpResponse& resp) {
    return resp.package_to_string(); // 将 HttpResponse 对象序列化为 HTTP 响应报文字符串
}

void HttpServer::send_data(const std::shared_ptr<TcpConnection>& conn, const std::string& response) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot send data, connection is nullptr");
        return;
    }
    conn->send(response); // 通过 TcpConnection 将响应数据写入发送缓冲区
}


void HttpServer::handle_http_callback(const HttpRequest& req, HttpResponse& resp) {
    if (!httpCallback) {
        spdlog::warn("HttpServer: HTTP callback not set, returning 404 Not Found");
        resp = generate_404_response(); // 未进行业务处理，并直接返回 404 响应
        return;
    }
    httpCallback(req, resp); // 调用上层业务回调，由业务层处理请求并填充响应
}

void HttpServer::check_and_set_content_length(HttpResponse& resp) {
    // 检查是否已设置 Content-Length，未设置则自动计算并添加
    auto& headers = const_cast<HttpResponse::Headers&>(resp.get_headers()); // 获取响应头（const_cast 用于修改）
    auto it = headers.find("Content-Length");
    if (it == headers.end()) {
        const std::string& body = resp.get_body();
        headers["Content-Length"] = std::to_string(body.size());
    }
}

HttpResponse HttpServer::generate_bad_response() {
    // 生成 400 Bad Request 响应（用于 HTTP 解析失败的情况）
    HttpResponse resp;
    resp.set_http_version("HTTP/1.1");
    resp.set_status(400, "Bad Request");
    resp.set_body("Bad Request");
    resp.add_header("Content-Type", "text/plain");
    resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
    resp.set_close_connection(true);
    return resp;
}

HttpResponse HttpServer::generate_404_response() {
    // 生成 404 Not Found 响应（用于未设置业务回调或路由未匹配的情况）
    HttpResponse resp;
    resp.set_http_version("HTTP/1.1");
    resp.set_status(404, "Not Found");
    resp.set_body("Not Found");
    resp.add_header("Content-Type", "text/plain");
    resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
    resp.set_close_connection(true);
    return resp;
}

