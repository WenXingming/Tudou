// ============================================== //
// HttpServer.cpp
// HTTP/HTTPS 服务门面，把连接事件拍平成读取、解析、执行业务、发送响应。
// ============================================== //

#include "HttpServer.h"
#include "HttpContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "SslContext.h"
#include "TlsConnection.h"

#include "spdlog/spdlog.h"
#include "tudou/tcp/TcpServer.h"
#include "tudou/tcp/TcpConnection.h"

namespace {

constexpr char kHttpVersion[] = "HTTP/1.1";
constexpr char kContentLengthHeader[] = "Content-Length";
constexpr char kContentTypeHeader[] = "Content-Type";
constexpr char kPlainTextContentType[] = "text/plain";
constexpr char kBadRequestMessage[] = "Bad Request";
constexpr char kNotFoundMessage[] = "Not Found";

} // namespace

HttpServer::HttpServer(std::string ip, uint16_t port, int threadNum) :
    ip_(std::move(ip)),
    port_(port),
    tcpServer_(std::make_unique<TcpServer>(this->ip_, this->port_, threadNum)),
    httpContexts_(),
    contextsMutex_(),
    messageCallback_(nullptr),
    sslContext_(nullptr),
    tlsConnections_() {

    bind_tcp_callbacks();
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
    sslContext_ = std::make_unique<SslContext>();
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

void HttpServer::process(const std::shared_ptr<TcpConnection>& conn) {
    // 业务门面：统一呈现为“收数据 -> 归一化传输层 -> 解析 HTTP -> 执行业务 -> 发送响应”的单向流程。
    std::string receivedData = receive_data(conn);
    if (receivedData.empty()) {
        return;
    }

    TransportPayload payload = normalize_transport_payload(conn, std::move(receivedData));
    if (!payload.ready) {
        return;
    }

    std::shared_ptr<HttpContext> context = resolve_request_context(conn);
    if (!context) {
        return;
    }

    const ParseState parseState = parse_http_request(*context, payload.plaintext);
    if (parseState == ParseState::NeedMoreData) {
        log_incomplete_request(conn->get_fd());
        return;
    }

    if (parseState == ParseState::Rejected) {
        reject_bad_request(conn, *context);
        return;
    }

    reply_complete_request(conn, *context);
}

void HttpServer::bind_tcp_callbacks() {
    // 事件回调只负责把 TcpServer 事件转发到 HTTP 门面，不再在 lambda 里编排业务细节。
    tcpServer_->set_connection_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        on_connect(conn);
        });
    tcpServer_->set_message_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        process(conn);
        });
    tcpServer_->set_close_callback([this](const std::shared_ptr<TcpConnection>& conn) {
        on_close(conn);
        });
}

void HttpServer::on_connect(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot create connection state, connection is nullptr");
        return;
    }

    const int fd = conn->get_fd();
    std::shared_ptr<HttpContext> httpContext = create_http_context();
    std::shared_ptr<TlsConnection> tlsConnection;
    if (is_ssl_enabled()) {
        tlsConnection = create_tls_connection(fd);
    }

    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        if (httpContexts_.find(fd) != httpContexts_.end()) {
            spdlog::warn("HttpServer: HttpContext already exists for fd={}, overwriting.", fd);
        }
        httpContexts_[fd] = std::move(httpContext);
        if (tlsConnection) {
            tlsConnections_[fd] = std::move(tlsConnection);
        }
    }

    spdlog::debug("HttpServer: New connection established, fd={}", fd);
}

std::shared_ptr<HttpContext> HttpServer::create_http_context() const {
    return std::make_shared<HttpContext>();
}

std::shared_ptr<TlsConnection> HttpServer::create_tls_connection(int fd) const {
    if (!sslContext_) {
        return nullptr;
    }

    SSL* ssl = sslContext_->create_ssl();
    if (!ssl) {
        spdlog::error("HttpServer: Failed to create SSL for fd={}", fd);
        return nullptr;
    }

    spdlog::debug("HttpServer: TlsConnection created for fd={}", fd);
    return std::make_shared<TlsConnection>(ssl);
}

void HttpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot close connection state, connection is nullptr");
        return;
    }

    const int fd = conn->get_fd();
    remove_http_context(fd);
    if (is_ssl_enabled()) {
        remove_tls_connection(fd);
    }
    spdlog::debug("HttpServer: Connection closed, fd={}", fd);
}

std::string HttpServer::receive_data(const std::shared_ptr<TcpConnection>& conn) const {
    if (!conn) {
        spdlog::error("HttpServer: Cannot receive data, connection is nullptr");
        return "";
    }

    return conn->receive();
}

HttpServer::TransportPayload HttpServer::normalize_transport_payload(const std::shared_ptr<TcpConnection>& conn,
                                                                    std::string receivedData) {
    if (!is_ssl_enabled()) {
        return normalize_plaintext_payload(std::move(receivedData));
    }

    return normalize_tls_payload(conn, receivedData);
}

HttpServer::TransportPayload HttpServer::normalize_plaintext_payload(std::string receivedData) const {
    TransportPayload payload;
    payload.ready = !receivedData.empty();
    payload.plaintext = std::move(receivedData);
    return payload;
}

HttpServer::TransportPayload HttpServer::normalize_tls_payload(const std::shared_ptr<TcpConnection>& conn,
                                                              const std::string& encryptedData) {
    TransportPayload payload;
    if (!conn) {
        return payload;
    }

    std::shared_ptr<TlsConnection> tls = find_tls_connection(conn->get_fd());
    if (!tls) {
        return payload;
    }

    if (!feed_tls_ciphertext(*tls, encryptedData, conn->get_fd())) {
        return payload;
    }

    if (tls->is_handshaking() && !advance_tls_handshake(*tls, conn->get_fd())) {
        return payload;
    }

    if (!flush_tls_output(conn, *tls)) {
        return payload;
    }

    if (tls->is_handshaking()) {
        return payload;
    }

    if (!tls->is_established()) {
        spdlog::error("HttpServer: TlsConnection in error state for fd={}", conn->get_fd());
        return payload;
    }

    return decrypt_tls_payload(*tls, conn->get_fd());
}

bool HttpServer::feed_tls_ciphertext(TlsConnection& tls, const std::string& encryptedData, int fd) const {
    if (tls.feed_data(encryptedData.data(), encryptedData.size()) < 0) {
        spdlog::error("HttpServer: TLS feed_data failed for fd={}", fd);
        return false;
    }

    return true;
}

bool HttpServer::advance_tls_handshake(TlsConnection& tls, int fd) const {
    if (!tls.do_handshake()) {
        spdlog::error("HttpServer: TLS handshake failed for fd={}", fd);
        return false;
    }

    return true;
}

bool HttpServer::flush_tls_output(const std::shared_ptr<TcpConnection>& conn, TlsConnection& tls) const {
    const std::string pendingOutput = tls.get_output();
    if (pendingOutput.empty()) {
        return true;
    }

    if (!conn) {
        spdlog::error("HttpServer: Cannot flush TLS output, connection is nullptr");
        return false;
    }

    // 握手包和应用密文都统一经由这里回写，避免 TLS 输出路径散落在多个分支里。
    conn->send(pendingOutput);
    return true;
}

HttpServer::TransportPayload HttpServer::decrypt_tls_payload(TlsConnection& tls, int fd) const {
    TransportPayload payload;
    std::string plaintext;
    const int decrypted = tls.decrypt(plaintext);
    if (decrypted < 0) {
        spdlog::error("HttpServer: TLS decrypt failed for fd={}", fd);
        return payload;
    }

    payload.ready = decrypted > 0;
    payload.plaintext = std::move(plaintext);
    return payload;
}

std::shared_ptr<HttpContext> HttpServer::find_http_context(int fd) {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = httpContexts_.find(fd);
    if (it == httpContexts_.end()) {
        spdlog::error("HttpServer: No HttpContext found for fd={}", fd);
        return nullptr;
    }

    return it->second;
}

std::shared_ptr<HttpContext> HttpServer::resolve_request_context(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot resolve HttpContext, connection is nullptr");
        return nullptr;
    }

    return find_http_context(conn->get_fd());
}

HttpServer::ParseState HttpServer::parse_http_request(HttpContext& ctx, const std::string& payload) const {
    size_t nparsed = 0;
    if (!ctx.parse(payload.data(), payload.size(), nparsed)) {
        return ParseState::Rejected;
    }

    if (!ctx.is_complete()) {
        return ParseState::NeedMoreData;
    }

    return ParseState::Complete;
}

void HttpServer::log_incomplete_request(int fd) const {
    spdlog::debug("HttpServer: HTTP request incomplete, waiting for more data, fd={}", fd);
}

void HttpServer::reject_bad_request(const std::shared_ptr<TcpConnection>& conn, HttpContext& ctx) {
    send_http_response(conn, build_bad_request_response());
    reset_http_context(ctx);
}

void HttpServer::reply_complete_request(const std::shared_ptr<TcpConnection>& conn, HttpContext& ctx) {
    send_http_response(conn, build_http_response(ctx.get_request()));
    reset_http_context(ctx);
}

HttpResponse HttpServer::build_http_response(const HttpRequest& req) const {
    HttpResponse response;
    if (!messageCallback_) {
        spdlog::warn("HttpServer: HTTP callback not set, returning 404 Not Found");
        return build_not_found_response();
    }

    // 业务层只负责填充领域响应，不需要关心默认 404、Content-Length 或 TLS 等基础设施细节。
    messageCallback_(req, response);
    return response;
}

HttpResponse HttpServer::build_bad_request_response() const {
    return build_plain_text_response(400, kBadRequestMessage, kBadRequestMessage);
}

HttpResponse HttpServer::build_not_found_response() const {
    return build_plain_text_response(404, kNotFoundMessage, kNotFoundMessage);
}

HttpResponse HttpServer::build_plain_text_response(int statusCode,
                                                   const std::string& statusMessage,
                                                   const std::string& body) const {
    HttpResponse resp;
    resp.set_http_version(kHttpVersion);
    resp.set_status(statusCode, statusMessage);
    resp.set_body(body);
    resp.set_header(kContentTypeHeader, kPlainTextContentType);
    resp.set_header(kContentLengthHeader, std::to_string(resp.get_body().size()));
    resp.set_close_connection(true);
    return resp;
}

void HttpServer::finalize_http_response(HttpResponse& resp) const {
    // Content-Length 是网络契约的一部分，统一在基础设施层补齐，避免业务回调重复关注协议细节。
    if (!resp.has_header(kContentLengthHeader)) {
        resp.set_header(kContentLengthHeader, std::to_string(resp.get_body().size()));
    }
}

std::string HttpServer::serialize_response(const HttpResponse& resp) const {
    return resp.package_to_string();
}

void HttpServer::send_http_response(const std::shared_ptr<TcpConnection>& conn, HttpResponse resp) {
    finalize_http_response(resp);
    send_response(conn, serialize_response(resp));
}

void HttpServer::send_response(const std::shared_ptr<TcpConnection>& conn, const std::string& response) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot send data, connection is nullptr");
        return;
    }

    if (is_ssl_enabled()) {
        // HTTPS 场景下发送前统一加密，保持业务侧始终操作明文 HttpResponse。
        const std::string encrypted = encrypt_response(conn->get_fd(), response);
        if (encrypted.empty()) {
            return;
        }

        conn->send(encrypted);
        return;
    }

    conn->send(response);
}

std::shared_ptr<TlsConnection> HttpServer::find_tls_connection(int fd) {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = tlsConnections_.find(fd);
    if (it == tlsConnections_.end()) {
        spdlog::error("HttpServer: No TlsConnection found for fd={}", fd);
        return nullptr;
    }

    return it->second;
}

std::string HttpServer::encrypt_response(int fd, const std::string& plainData) {
    std::shared_ptr<TlsConnection> tls = find_tls_connection(fd);
    if (!tls) {
        return "";
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

void HttpServer::reset_http_context(HttpContext& ctx) const {
    ctx.reset();
}

void HttpServer::remove_http_context(int fd) {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = httpContexts_.find(fd);
    if (it == httpContexts_.end()) {
        spdlog::warn("HttpServer: No HttpContext found for fd={} on close.", fd);
        return;
    }

    httpContexts_.erase(it);
}

void HttpServer::remove_tls_connection(int fd) {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = tlsConnections_.find(fd);
    if (it == tlsConnections_.end()) {
        return;
    }

    tlsConnections_.erase(it);
    spdlog::debug("HttpServer: TlsConnection removed for fd={}", fd);
}

