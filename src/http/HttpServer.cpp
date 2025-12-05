#include "HttpServer.h"

#include "spdlog/spdlog.h"


HttpServer::HttpServer(std::string ip, uint16_t port)
    : ip(std::move(ip))
    , port(port) {

    tcpServer.reset(new TcpServer(this->ip, this->port));
    tcpServer->set_connection_callback(
        [this](const std::shared_ptr<TcpConnection>& conn) {
            connect_callback(conn);
        });
    tcpServer->set_message_callback(
        [this](const std::shared_ptr<TcpConnection>& conn) {
            message_callback(conn);
        });
    tcpServer->set_close_callback(
        [this](const std::shared_ptr<TcpConnection>& conn) {
            close_callback(conn);
        });
}

void HttpServer::start() {
    tcpServer->start();
}

void HttpServer::connect_callback(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    httpContexts[fd].reset(new HttpContext());
    spdlog::debug("HttpServer: on_connection fd={}", fd);
}

void HttpServer::message_callback(const std::shared_ptr<TcpConnection>& conn) {
    spdlog::debug("HttpServer: on_message fd={}", conn->get_fd());

    int fd = conn->get_fd();
    auto it = httpContexts.find(fd);
    if (it == httpContexts.end()) {
        spdlog::error("HttpServer: No HttpContext found for fd={}", fd);
        return;
    }
    HttpContext& ctx = *(it->second);

    // 目前 TcpConnection 只暴露 receive()，先用一次性读取实现简单版本
    // std::string data = conn->receive();
    std::string data;
    data.reserve(4096); // 预分配一些空间，避免多次 realloc
    while (true) {
        std::string chunk = conn->receive();
        if (chunk.empty()) break;
        data.append(std::move(chunk));
    }
    if (data.empty()) {
        spdlog::debug("HttpServer: Received empty data from fd={}", fd);
        return;
    }

    size_t nparsed = 0;
    bool ok = ctx.parse(data.data(), data.size(), nparsed);
    if (!ok) {
        spdlog::debug("HttpServer: Bad request from fd={}", fd);
        conn->send(generate_bad_response());
        // conn->shutdown();
        return;
    }

    if (ctx.is_complete()) {
        handle_message(conn, ctx);
        ctx.reset();
    }
}


void HttpServer::close_callback(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    httpContexts.erase(fd);
    spdlog::debug("HttpServer: Connection closed. fd={}", fd);
}


void HttpServer::handle_message(const std::shared_ptr<TcpConnection>& conn,
    HttpContext& ctx) {
    if (!messageCallback) {
        conn->send(generate_404_response()); // 默认返回 404
        // conn->shutdown();
        return;
    }
    const HttpRequest& req = ctx.get_request();
    HttpResponse resp;
    this->messageCallback(req, resp); // 调用业务回调生成响应

    // 如果业务没设置 Content-Length，这里自动补一份
    auto& headers = const_cast<HttpResponse::Headers&>(resp.get_headers());
    auto findIt = headers.find("Content-Length");
    if (findIt == headers.end()) {
        const std::string& body = resp.get_body();
        headers["Content-Length"] = std::to_string(body.size());
    }
    std::string response_str = resp.package_to_string();
    conn->send(response_str);


    if (resp.get_close_connection()) {
        // conn->shutdown();
    }
}


std::string HttpServer::generate_bad_response() {
    HttpResponse resp;
    resp.set_status(400, "Bad Request");
    resp.set_close_connection(true);
    resp.set_body("Bad Request");
    resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
    resp.add_header("Content-Type", "text/plain");
    return std::move(resp.package_to_string());
}

std::string HttpServer::generate_404_response() {
    HttpResponse resp;
    resp.set_status(404, "Not Found");
    resp.set_close_connection(true);
    resp.set_body("Not Found");
    resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
    resp.add_header("Content-Type", "text/plain");
    return std::move(resp.package_to_string());
}

