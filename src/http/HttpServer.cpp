#include "HttpServer.h"

#include "../base/Log.h"
#include "spdlog/spdlog.h"
namespace tudou {

    HttpServer::HttpServer(EventLoop* loop, const InetAddress& listen_addr)
        : loop(loop),
        tcpServer(loop, listen_addr) {
        tcpServer.set_connection_callback(
            [this](const std::shared_ptr<TcpConnection>& conn) {
                on_connection(conn);
            });

        tcpServer.set_message_callback(
            [this](const std::shared_ptr<TcpConnection>& conn) {
                on_message(conn);
            });
    }

    void HttpServer::start() {

    }

    void HttpServer::on_connection(const std::shared_ptr<TcpConnection>& conn) {
        int fd = conn->get_fd();
        if (/* conn->is_connected() */1) {
            // LOG::LOG_DEBUG("[HttpServer] New connection established. fd=%d", fd);
            spdlog::debug("[HttpServer] New connection established. fd={}", fd);

            // contexts.emplace(fd, HttpContext{});
            contexts[fd].reset(new HttpContext());
        }
        else {
            // LOG::LOG_ERROR("[HttpServer] Connection closed. fd=%d", fd);
            contexts.erase(fd);
        }
    }

    void HttpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
        // LOG::LOG_DEBUG("[HttpServer] on_message fd=%d", conn->get_fd());

        int fd = conn->get_fd();
        auto it = contexts.find(fd);
        if (it == contexts.end()) {
            // 没有上下文，直接丢弃本次数据
            // conn->receive();
            // return;
            // 没有上下文就创建一个，避免丢失
            // it = contexts.emplace(fd, HttpContext{}).first;
            it->second.reset(new HttpContext());
            // LOG::LOG_DEBUG("[HttpServer] Created new HttpContext for fd=%d", fd);
        }

        HttpContext& ctx = *(it->second);

        // 目前 TcpConnection 只暴露 receive()，先用一次性读取实现简单版本
        // std::string data = conn->receive();
        std::string data;
        data.reserve(4096); // 预分配一些空间，避免多次 realloc
        while (true) {
            std::string chunk = conn->receive();
            if (chunk.empty()) break;
            data.append(chunk);
        }
        // LOG::LOG_DEBUG("[HttpServer] data size = %zu", data.size());
        if (data.empty()) {
            return;
        }

        size_t nparsed = 0;
        // LOG::LOG_DEBUG("[HttpServer] before parse");
        bool ok = ctx.parse(data.data(), data.size(), nparsed);
        // LOG::LOG_DEBUG("[HttpServer] after parse");
        // LOG::LOG_DEBUG("[HttpServer] parse ok=%d complete=%d nparsed=%zu len=%zu", ok, ctx.is_complete(), nparsed, data.size());
        if (!ok) {
            // LOG::LOG_ERROR("[HttpServer] parse error, close connection");
            HttpResponse resp;
            resp.set_status(400, "Bad Request");
            resp.set_close_connection(true);
            resp.set_body("Bad Request");
            resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
            resp.add_header("Content-Type", "text/plain");
            conn->send(resp.package_to_string());
            // conn->shutdown();
            return;
        }

        if (ctx.is_complete()) {
            handle_request(conn, ctx);
            ctx.reset();
        }
    }

    void HttpServer::handle_request(const std::shared_ptr<TcpConnection>& conn,
        HttpContext& ctx) {
        if (!httpCallback) {
            conn->send(generate_404_page()); // 默认返回 404
            // conn->shutdown();
            return;
        }
        const HttpRequest& req = ctx.get_request();
        HttpResponse resp;
        this->httpCallback(req, resp); // 调用业务回调生成响应

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

    std::string HttpServer::generate_404_page() {
        HttpResponse resp;
        resp.set_status(404, "Not Found");
        resp.set_close_connection(true);
        resp.set_body("Not Found");
        resp.add_header("Content-Length", std::to_string(resp.get_body().size()));
        resp.add_header("Content-Type", "text/plain");
        return std::move(resp.package_to_string());
    }

} // namespace tudou
