// ============================================================================
// HTTP/HTTPS 服务门面实现，按读取、解密、解析、路由、发送的顺序处理连接事件。
// TLS 仅使用 Memory BIO；Socket I/O 仍由 TcpConnection 和 EventLoop 负责。
// ============================================================================

#include "tudou/http/HttpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/TlsConfig.h"
#include "tudou/http/TlsConnection.h"

#include <vector>

#include "spdlog/spdlog.h"
#include "tudou/tcp/TcpServer.h"

namespace {

constexpr char kBadRequestMessage[] = "Bad Request";

} // namespace

HttpServer::HttpServer(std::string ip, uint16_t port, int threadNum) :
    ip_(std::move(ip)),
    port_(port),
    tcpServer_(ip_, port_, threadNum),
    httpConnections_(),
    contextsMutex_(),
    router_(),
    tlsConfig_(nullptr) {

    // 事件回调只负责把 TcpServer 事件转发到 HTTP 门面，不再在 lambda 里编排业务细节
    tcpServer_.set_connection_callback([this](const TcpConnectionPtr& conn) {
        on_connect(conn);
        });
    tcpServer_.set_message_callback([this](const TcpConnectionPtr& conn) {
        on_message(conn);
        });
    tcpServer_.set_close_callback([this](const TcpConnectionPtr& conn) {
        on_close(conn);
        });
}

void HttpServer::start() {
    if (tlsConfig_) {
        spdlog::info("HttpServer: Starting HTTPS server at {}:{}", ip_, port_);
    }
    else {
        spdlog::debug("HttpServer: Starting HTTP server at {}:{}", ip_, port_);
    }

    tcpServer_.start();
}

void HttpServer::add_route(const std::string& method, const std::string& path, Handler handler) {
    router_.add_route(method, path, std::move(handler));
}

void HttpServer::add_get_route(const std::string& path, Handler handler) {
    router_.add_get_route(path, std::move(handler));
}

void HttpServer::add_post_route(const std::string& path, Handler handler) {
    router_.add_post_route(path, std::move(handler));
}

void HttpServer::add_head_route(const std::string& path, Handler handler) {
    router_.add_head_route(path, std::move(handler));
}

void HttpServer::add_prefix_route(const std::string& prefix, Handler handler) {
    router_.add_prefix_route(prefix, std::move(handler));
}

bool HttpServer::enable_ssl(const std::string& certFile, const std::string& keyFile) {
    tlsConfig_ = std::make_unique<TlsConfig>();
    if (!tlsConfig_->init(certFile, keyFile)) {
        spdlog::critical("HttpServer: Failed to initialize TLS configuration");
        tlsConfig_.reset();
        return false;
    }

    spdlog::info("HttpServer: SSL enabled (cert={}, key={})", certFile, keyFile);
    return true;
}

void HttpServer::on_connect(const TcpConnectionPtr& conn) {
    // 创建 HttpConnection，可能包含 TLS 状态
    std::shared_ptr<HttpConnection> httpConnection;
    if (tlsConfig_) {
        SSL* ssl = tlsConfig_->create_ssl_session();
        if (!ssl) {
            spdlog::error("HttpServer: Failed to create SSL for fd={}", conn->get_fd());
            conn->force_close();
            return;
        }

        spdlog::debug("HttpServer: TlsConnection created for fd={}", conn->get_fd());
        httpConnection = std::make_shared<HttpConnection>(std::make_unique<TlsConnection>(ssl));
    }
    else {
        httpConnection = std::make_shared<HttpConnection>();
    }
    // 注册到全局数据结构
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        httpConnections_.emplace(conn.get(), std::move(httpConnection));
    }

    spdlog::debug("HttpServer: New connection established, fd={}", conn ? conn->get_fd() : -1);
}

void HttpServer::on_message(const TcpConnectionPtr& conn) {
    // 取出 TcpConnection 本轮读到的网络字节；空数据无需进入协议层。
    const std::string receivedData = conn ? conn->receive() : std::string();
    if (receivedData.empty()) {
        return;
    }

    // 从共享连接表取出状态。复制 shared_ptr 后立即释锁，on_close() 不会在处理期间析构它。
    std::shared_ptr<HttpConnection> httpConnection;
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        const auto it = httpConnections_.find(conn.get());
        if (it == httpConnections_.end()) {
            spdlog::error("HttpServer: No HttpConnection found for fd={}", conn ? conn->get_fd() : -1);
            return;
        }
        httpConnection = it->second;
    }

    // 完成 TLS 解密、HTTP 增量解析与 pipeline 请求提取；TLS 握手回包须先写回客户端。
    std::vector<HttpRequest> requests;
    std::string outboundCiphertext;
    const HttpConnection::ProcessResult processResult =
        httpConnection->decode_requests(receivedData, requests, outboundCiphertext);
    if (!outboundCiphertext.empty()) {
        conn->send(outboundCiphertext);
    }
    if (processResult == HttpConnection::ProcessResult::TlsError) {
        spdlog::error("HttpServer: TLS read failed for fd={}", conn ? conn->get_fd() : -1);
        conn->force_close();
        return;
    }

    // 按请求在字节流中的顺序路由并发送响应；任一响应要求关闭时停止后续处理。
    for (const HttpRequest& request : requests) {
        HttpResponse response;
        router_.dispatch(request, response);
        if (send_http_response(conn, *httpConnection, response)) {
            return;
        }
    }

    // 前面已解析成功的 pipeline 请求已完成响应；当前错误请求以 400 收口连接。
    if (processResult == HttpConnection::ProcessResult::BadRequest) {
        HttpResponse response;
        response.set_status(400, kBadRequestMessage);
        response.set_header("Content-Type", "text/plain");
        response.set_body(kBadRequestMessage);
        response.set_header("Connection", "close");
        send_http_response(conn, *httpConnection, response);
    }
}

void HttpServer::on_close(const TcpConnectionPtr& conn) {
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        httpConnections_.erase(conn.get());
    }
    spdlog::debug("HttpServer: Connection closed, fd={}", conn ? conn->get_fd() : -1);
}

bool HttpServer::send_http_response(const TcpConnectionPtr& conn, HttpConnection& httpConnection, HttpResponse resp) {
    if (!conn) {
        return false;
    }

    const auto connectionHeader = resp.get_headers().find("Connection");
    const bool closeConnection = connectionHeader != resp.get_headers().end()
        && connectionHeader->second == "close";

    // 将响应编码为网络字节，再交给 TcpConnection 发送。
    std::string networkData;
    if (!httpConnection.encode_response(resp, networkData)) {
        spdlog::error("HttpServer: Failed to encode HTTP response, fd={}", conn->get_fd());
        conn->force_close();
        return true;
    }
    if (!networkData.empty()) {
        conn->send(networkData);
    }

    // Connection: close 是响应协议语义，发送后由连接层执行关闭。
    if (closeConnection) {
        conn->force_close();
    }
    return closeConnection;
}
