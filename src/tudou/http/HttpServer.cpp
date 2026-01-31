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

// ==================== 公共接口 ====================

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

// ==================== TcpServer 回调处理 ====================

void HttpServer::on_connect(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    spdlog::debug("HttpServer: New connection established, fd={}", fd);

    // 为新连接创建 HttpContext，用于后续的 HTTP 报文解析
    std::lock_guard<std::mutex> lock(contextsMutex);
    httpContexts[fd].reset(new HttpContext());  // 兼容 C++11：不使用 std::make_unique
}

void HttpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    spdlog::debug("HttpServer: Message received, fd={}", fd);

    // 启动 HTTP 处理流程
    // 步骤 1：接收原始数据
    std::string data = receive_data(conn);

    // 步骤 2~5：解析 HTTP 请求 -> 业务处理 -> 打包响应 -> 发送
    parse_receive_data(conn, data);
}

void HttpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    spdlog::debug("HttpServer: Connection closed, fd={}", fd);

    // 删除该连接对应的 HttpContext
    std::lock_guard<std::mutex> lock(contextsMutex);
    httpContexts.erase(fd);
}

// ==================== HTTP 请求处理流程（5 个步骤）====================

// 步骤 1：接收数据
std::string HttpServer::receive_data(const std::shared_ptr<TcpConnection>& conn) {
    // 从 TcpConnection 的读缓冲区获取原始数据
    return conn->receive();
}

// 步骤 2：解析 HTTP 请求
void HttpServer::parse_receive_data(const std::shared_ptr<TcpConnection>& conn, const std::string& data) {
    int fd = conn->get_fd();

    // 获取该连接对应的 HttpContext（使用 shared_ptr 拷贝缩小锁的临界区）
    std::shared_ptr<HttpContext> ctx;
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        auto it = httpContexts.find(fd);
        if (it == httpContexts.end()) {
            spdlog::error("HttpServer: No HttpContext found for fd={}", fd);
            return;
        }
        ctx = it->second;  // 拷贝 shared_ptr，保证在锁外使用时对象仍然存活
    }

    // 解析 HTTP 请求报文
    size_t nparsed = 0;
    bool ok = ctx->parse(data.data(), data.size(), nparsed);

    if (!ok) {
        // 解析失败，返回 400 Bad Request
        spdlog::warn("HttpServer: Failed to parse HTTP request from fd={}", fd);
        HttpResponse resp = generate_bad_response();
        send_data(conn, resp.package_to_string());
        ctx->reset();
        return;
    }

    // 检查 HTTP 请求是否完整
    if (!ctx->is_complete()) {
        // 请求未完整接收，等待更多数据（支持长连接场景）
        spdlog::debug("HttpServer: HTTP request incomplete, waiting for more data, fd={}", fd);
        return;
    }

    spdlog::debug("HttpServer: Complete HTTP request parsed successfully, fd={}", fd);

    // 步骤 3~5：处理完整的 HTTP 请求
    process_data(conn, *ctx);
}

// 步骤 3：处理业务逻辑
void HttpServer::process_data(const std::shared_ptr<TcpConnection>& conn, HttpContext& ctx) {
    // 获取解析完成的 HTTP 请求
    const HttpRequest& req = ctx.get_request();

    // 创建响应对象并调用业务回调
    HttpResponse resp;
    handle_http_callback(req, resp);

    // 自动补充 Content-Length 头（如果业务层未设置）
    check_and_set_content_length(resp);

    // 步骤 4：打包 HTTP 响应
    std::string respStr = package_response_data(resp);

    // 步骤 5：发送响应数据
    send_data(conn, respStr);

    // 重置 HttpContext，为处理同一连接的下一个请求做准备（支持长连接）
    ctx.reset();
}

// 步骤 4：打包 HTTP 响应
std::string HttpServer::package_response_data(const HttpResponse& resp) {
    // 将 HttpResponse 对象序列化为 HTTP 响应报文字符串
    return resp.package_to_string();
}

// 步骤 5：发送响应数据
void HttpServer::send_data(const std::shared_ptr<TcpConnection>& conn, const std::string& response) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot send data, connection is nullptr");
        return;
    }
    // 通过 TcpConnection 将响应数据写入发送缓冲区
    conn->send(response);
}

// ==================== HTTP 业务逻辑辅助 ====================

void HttpServer::handle_http_callback(const HttpRequest& req, HttpResponse& resp) {
    if (!httpCallback) {
        // 未设置业务回调，返回默认 404 响应
        spdlog::warn("HttpServer: HTTP callback not set, returning 404 Not Found");
        resp = generate_404_response();
        return;
    }

    // 调用上层业务回调，由业务层处理请求并填充响应
    httpCallback(req, resp);
}

void HttpServer::check_and_set_content_length(HttpResponse& resp) {
    // 获取响应头（const_cast 用于修改）
    auto& headers = const_cast<HttpResponse::Headers&>(resp.get_headers());

    // 检查是否已设置 Content-Length
    auto it = headers.find("Content-Length");
    if (it == headers.end()) {
        // 未设置则自动计算并添加
        const std::string& body = resp.get_body();
        headers["Content-Length"] = std::to_string(body.size());
    }
}

// ==================== HTTP 默认响应生成 ====================

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

