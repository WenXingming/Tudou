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
#include "tudou/TcpServer.h"


HttpServer::HttpServer(std::string _ip, uint16_t _port, int _threadNum) :
    ip(std::move(_ip)),
    port(_port),
    threadNum(_threadNum),
    tcpServer(nullptr),
    httpContexts(),
    contextsMutex(),
    httpCallback(nullptr) {

    tcpServer.reset(new TcpServer(this->ip, this->port, this->threadNum));
    tcpServer->set_connection_callback(
        [this](int fd) {
            on_connect(fd);
        });
    tcpServer->set_message_callback(
        [this](int fd, const std::string& msg) {
            on_message(fd, msg);
        });
    tcpServer->set_close_callback(
        [this](int fd) {
            on_close(fd);
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

void HttpServer::on_connect(int fd) {
    spdlog::debug("HttpServer: on_connection fd={}", fd);
    {
    std::lock_guard<std::mutex> lock(contextsMutex);
    httpContexts[fd].reset(new HttpContext()); // 兼容 C++11：不使用 std::make_unique
    }
}

void HttpServer::on_message(int fd, const std::string& msg) {
    spdlog::debug("HttpServer: on_message fd={}", fd);

    // 第 1 步：接收数据（目前直接使用 msg）
    std::string data = std::move(receive_data(msg));

    // 第 2~4 步：解析 + 处理业务 + 打包响应
    parse_receive_data(fd, data);
}

void HttpServer::on_close(int fd) {
    spdlog::debug("HttpServer: Connection closed. fd={}", fd);
    std::lock_guard<std::mutex> lock(contextsMutex);
    httpContexts.erase(fd);
}

std::string HttpServer::receive_data(const std::string& data) {
    // 当前 TcpServer 已经一次性从 TcpConnection 读取到了数据，这里直接返回。
    // 保留该函数主要是为了保持整体的五步流程语义清晰。
    return data;
}

void HttpServer::parse_receive_data(int fd, const std::string& data) {
    // 1. 在锁内只做 HttpContext 查找和 shared_ptr 拷贝，缩小临界区
    std::shared_ptr<HttpContext> ctx;
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        auto it = httpContexts.find(fd);
        if (it == httpContexts.end()) {
            spdlog::error("HttpServer: No HttpContext found for fd={}", fd);
            return;
        }
        ctx = it->second; // 拷贝 shared_ptr，保证在锁外使用时对象仍然存活
    }

    size_t nparsed = 0;
    bool ok = ctx->parse(data.data(), data.size(), nparsed);
    if (!ok) {
        // 解析失败，返回 400 Bad Request
        spdlog::debug("HttpServer::parse_receive_data wrong. Bad request from fd={}", fd);
        HttpResponse resp = generate_bad_response();
        send_data(fd, resp.package_to_string());
        ctx->reset();
        return;
    }

    // 短连接场景下一般一次就收完；长连接场景可多次累积，这里先简单返回等待更多数据
    if (!ctx->is_complete()) {
        spdlog::debug("HttpServer::parse_receive_data. HTTP request not complete yet, fd={}", fd);
        return;
    }
    spdlog::debug("HttpServer::parse_receive_data ok. Complete HTTP request received from fd={}", fd);

    // 报文完整，进入业务处理逻辑
    process_data(fd, *ctx);
}

void HttpServer::process_data(int fd, HttpContext& ctx) {
    // 调用上层业务回调处理请求并构造响应
    const HttpRequest& req = ctx.get_request();
    HttpResponse resp;
    handle_http_callback(req, resp);

    // 如果业务方没有手动设置 Content-Length，这里自动补充
    check_and_set_content_length(resp);

    // 打包响应并发送
    std::string respStr = package_response_data(resp);
    send_data(fd, respStr);

    // 当前 HttpContext 只处理一个完整请求，处理完后复用同一个 context 以便后续请求
    ctx.reset();
}

std::string HttpServer::package_response_data(const HttpResponse& resp) {
    return resp.package_to_string();
}

void HttpServer::send_data(int fd, const std::string& response) {
    if (!tcpServer) {
        spdlog::error("HttpServer::send_data. tcpServer is nullptr, fd={}", fd);
        return;
    }
    tcpServer->send_message(fd, response);
}

void HttpServer::handle_http_callback(const HttpRequest& req, HttpResponse& resp) {
    if (!httpCallback) {
        spdlog::critical("HttpServer: No HTTP callback set. Use default 404 response.");
        resp = generate_404_response(); // 直接填充 404 响应
        return;
    }

    httpCallback(req, resp); // 调用上层业务回调，处理请求，填充响应
}

void HttpServer::check_and_set_content_length(HttpResponse & resp){
    auto& headers = const_cast<HttpResponse::Headers&>(resp.get_headers());
    auto it = headers.find("Content-Length");
    if (it == headers.end()) {
        const std::string& body = resp.get_body();
        headers["Content-Length"] = std::to_string(body.size());
    }
}

HttpResponse HttpServer::generate_bad_response() {
    HttpResponse resp;
    resp.set_http_version("HTTP/1.1");
    resp.set_status(400, "Bad Request");
    resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
    resp.add_header("Content-Type", "text/plain");
    resp.set_body("Bad Request");
    resp.set_close_connection(true);
    return std::move(resp);
}

HttpResponse HttpServer::generate_404_response() {
    HttpResponse resp;
    resp.set_http_version("HTTP/1.1");
    resp.set_status(404, "Not Found");
    resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
    resp.add_header("Content-Type", "text/plain");
    resp.set_body("Not Found");
    resp.set_close_connection(true);
    return std::move(resp);
}

