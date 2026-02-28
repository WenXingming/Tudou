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
#include "SslContext.h"
#include "TlsConnection.h"

#include "spdlog/spdlog.h"
#include "tudou/tcp/TcpServer.h"
#include "tudou/tcp/TcpConnection.h"

 // ==================== 构造与析构 ====================

HttpServer::HttpServer(std::string ip, uint16_t port, int threadNum) :
    ip_(std::move(ip)),
    port_(port),
    tcpServer_(nullptr),
    httpContexts_(),
    contextsMutex_(),
    messageCallback_(nullptr),
    sslContext_(nullptr),
    tlsConnections_() {

    // 创建底层 TcpServer
    tcpServer_.reset(new TcpServer(this->ip_, this->port_, threadNum));

    // 注册 TcpServer 事件回调
    tcpServer_->set_connection_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        on_connect(conn);
        });
    tcpServer_->set_message_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        on_message(conn);
        });
    tcpServer_->set_close_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        on_close(conn);
        });
}

void HttpServer::start() {
    if (sslContext_ && sslContext_->is_initialized()) {
        spdlog::info("HttpServer: Starting HTTPS server at {}:{}", ip_, port_);
    }
    else {
        spdlog::debug("HttpServer: Starting HTTP server at {}:{}", ip_, port_);
    }
    if (!tcpServer_) {
        spdlog::critical("HttpServer: tcpServer is nullptr, cannot start server.");
        return;
    }
    tcpServer_->start();
}

void HttpServer::set_http_callback(const MessageCallback& cb) {
    messageCallback_ = cb;
}

bool HttpServer::enable_ssl(const std::string& certFile, const std::string& keyFile) {
    sslContext_.reset(new SslContext());
    if (!sslContext_->init(certFile, keyFile)) {
        spdlog::critical("HttpServer: Failed to initialize SSL context");
        sslContext_.reset();
        return false;
    }
    spdlog::info("HttpServer: SSL enabled (cert={}, key={})", certFile, keyFile);
    return true;
}

bool HttpServer::is_ssl_enabled() const {
    return sslContext_ && sslContext_->is_initialized();
}

const std::string& HttpServer::get_ip() const {
    return ip_;
}

int HttpServer::get_port() const {
    return port_;
}

int HttpServer::get_thread_num() const {
    return tcpServer_ ? tcpServer_->get_num_threads() : 0;
}

void HttpServer::on_connect(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        if (httpContexts_.find(fd) != httpContexts_.end()) {
            spdlog::warn("HttpServer: HttpContext already exists for fd={}, overwriting.", fd);
        }
        httpContexts_[fd] = std::shared_ptr<HttpContext>(new HttpContext()); // 这里的逻辑是如果存在则覆盖

        // 如果启用了 SSL，为该连接创建 TlsConnection
        if (is_ssl_enabled()) {
            tls_on_connect(fd);
        }

        spdlog::debug("HttpServer: New connection established, fd={}", fd);
    }
}

void HttpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();

    if (is_ssl_enabled()) {
        // HTTPS 模式：走 TLS 处理流程
        tls_on_message(conn, fd);
    }
    else {
        // HTTP 模式：原有明文处理流程
        std::string receivedData = receive_data(conn); // 启动 HTTP 处理流程。步骤 1：接收原始数据
        parse_received_data(conn, receivedData); // 步骤 2~5：解析 HTTP 请求 -> 业务处理 -> 打包响应 -> 发送
    }
}

void HttpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        if (httpContexts_.find(fd) == httpContexts_.end()) {
            spdlog::warn("HttpServer: No HttpContext found for fd={} on close.", fd);
            return;
        }
        httpContexts_.erase(fd);

        // 清理 TLS 连接状态
        if (is_ssl_enabled()) {
            tls_on_close(fd);
        }

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
        std::lock_guard<std::mutex> lock(contextsMutex_);
        auto it = httpContexts_.find(fd);
        if (it == httpContexts_.end()) {
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

    if (is_ssl_enabled()) {
        // HTTPS 模式：先加密再发送
        std::string encrypted = tls_encrypt(conn->get_fd(), response);
        if (!encrypted.empty()) {
            conn->send(encrypted);
        }
    }
    else {
        // HTTP 模式：直接发送明文
        conn->send(response);
    }
}


void HttpServer::handle_http_callback(const HttpRequest& req, HttpResponse& resp) {
    if (!messageCallback_) {
        spdlog::warn("HttpServer: HTTP callback not set, returning 404 Not Found");
        resp = generate_404_response(); // 未进行业务处理，并直接返回 404 响应
        return;
    }
    messageCallback_(req, resp); // 调用上层业务回调，由业务层处理请求并填充响应
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

// ==================== TLS 相关处理 ====================

void HttpServer::tls_on_connect(int fd) {
    // 在 contextsMutex_ 锁内部调用（由 on_connect 持锁）
    SSL* ssl = sslContext_->create_ssl();
    if (!ssl) {
        spdlog::error("HttpServer: Failed to create SSL for fd={}", fd);
        return;
    }
    tlsConnections_[fd] = std::shared_ptr<TlsConnection>(new TlsConnection(ssl));
    spdlog::debug("HttpServer: TlsConnection created for fd={}", fd);
}

void HttpServer::tls_on_message(const std::shared_ptr<TcpConnection>& conn, int fd) {
    // 步骤 1：接收密文数据
    std::string encryptedData = receive_data(conn);
    if (encryptedData.empty()) return;

    // 获取 TlsConnection
    std::shared_ptr<TlsConnection> tls;
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        auto it = tlsConnections_.find(fd);
        if (it == tlsConnections_.end()) {
            spdlog::error("HttpServer: No TlsConnection found for fd={}", fd);
            return;
        }
        tls = it->second;
    }

    // 将密文喂入 TLS
    int fed = tls->feed_data(encryptedData.data(), encryptedData.size());
    if (fed < 0) {
        spdlog::error("HttpServer: TLS feed_data failed for fd={}", fd);
        return;
    }

    // 根据 TLS 状态处理
    if (tls->is_handshaking()) {
        // TLS 握手阶段
        if (!tls->do_handshake()) {
            spdlog::error("HttpServer: TLS handshake failed for fd={}", fd);
            // 握手失败，发送 bad response 或关闭连接
            return;
        }

        // 取出握手响应数据并发回客户端
        std::string handshakeOutput = tls->get_output();
        if (!handshakeOutput.empty()) {
            conn->send(handshakeOutput);  // 握手数据直接发送（已经是 TLS 协议数据）
        }

        // 握手完成后，可能 SSL_do_handshake 已经消费了应用数据
        // 尝试读取握手完成后紧跟的应用数据
        if (tls->is_established()) {
            spdlog::debug("HttpServer: TLS handshake completed for fd={}, checking for pending data", fd);
            std::string plaintext;
            int decrypted = tls->decrypt(plaintext);
            if (decrypted > 0) {
                parse_received_data(conn, plaintext);
            }
        }
        return;
    }

    if (tls->is_established()) {
        // TLS 已建立，解密数据并按 HTTP 流程处理
        std::string plaintext;
        int decrypted = tls->decrypt(plaintext);
        if (decrypted < 0) {
            spdlog::error("HttpServer: TLS decrypt failed for fd={}", fd);
            return;
        }
        if (decrypted > 0) {
            parse_received_data(conn, plaintext);
        }
        return;
    }

    // TLS 错误状态
    spdlog::error("HttpServer: TlsConnection in error state for fd={}", fd);
}

void HttpServer::tls_on_close(int fd) {
    // 在 contextsMutex_ 锁内部调用（由 on_close 持锁）
    auto it = tlsConnections_.find(fd);
    if (it != tlsConnections_.end()) {
        tlsConnections_.erase(it);
        spdlog::debug("HttpServer: TlsConnection removed for fd={}", fd);
    }
}

std::string HttpServer::tls_decrypt(int fd, const std::string& encryptedData) {
    std::shared_ptr<TlsConnection> tls;
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        auto it = tlsConnections_.find(fd);
        if (it == tlsConnections_.end()) {
            spdlog::error("HttpServer: No TlsConnection found for fd={} in decrypt", fd);
            return "";
        }
        tls = it->second;
    }

    tls->feed_data(encryptedData.data(), encryptedData.size());

    std::string plaintext;
    tls->decrypt(plaintext);
    return plaintext;
}

std::string HttpServer::tls_encrypt(int fd, const std::string& plainData) {
    std::shared_ptr<TlsConnection> tls;
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        auto it = tlsConnections_.find(fd);
        if (it == tlsConnections_.end()) {
            spdlog::error("HttpServer: No TlsConnection found for fd={} in encrypt", fd);
            return "";
        }
        tls = it->second;
    }

    if (!tls->is_established()) {
        spdlog::error("HttpServer: Cannot encrypt, TLS not established for fd={}", fd);
        return "";
    }

    int written = tls->encrypt(plainData.data(), plainData.size());
    if (written <= 0) {
        spdlog::error("HttpServer: TLS encrypt failed for fd={}", fd);
        return "";
    }

    return tls->get_output();
}

