// ============================================================================
// HttpServer.cpp
// HTTP/HTTPS 服务门面，把连接事件拍平成读取、解析、执行业务、发送响应。
// ============================================================================

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

constexpr char kContentLengthHeader[] = "Content-Length";
constexpr char kBadRequestMessage[] = "Bad Request";
constexpr char kNotFoundMessage[] = "Not Found";

std::string normalize_plaintext_payload(std::string receivedData) {
    return receivedData;
}

bool feed_tls_ciphertext(TlsConnection& tls, const std::string& encryptedData, int fd) {
    if (tls.feed_data(encryptedData.data(), encryptedData.size()) < 0) {
        spdlog::error("HttpServer: TLS feed_data failed for fd={}", fd);
        return false;
    }

    return true;
}

bool advance_tls_handshake(TlsConnection& tls, int fd) {
    if (!tls.do_handshake()) {
        spdlog::error("HttpServer: TLS handshake failed for fd={}", fd);
        return false;
    }

    return true;
}

bool flush_tls_output(const std::shared_ptr<TcpConnection>& conn, TlsConnection& tls) {
    const std::string pendingOutput = tls.get_output();
    if (pendingOutput.empty()) {
        return true;
    }

    if (!conn) {
        spdlog::error("HttpServer: Cannot flush TLS output, connection is nullptr");
        return false;
    }

    conn->send(pendingOutput);
    return true;
}

std::string decrypt_tls_payload(TlsConnection& tls, int fd) {
    std::string plaintext;
    const int decrypted = tls.decrypt(plaintext);
    if (decrypted < 0) {
        spdlog::error("HttpServer: TLS decrypt failed for fd={}", fd);
        return "";
    }

    if (decrypted == 0) {
        return "";
    }

    return plaintext;
}

std::string normalize_tls_payload(const std::shared_ptr<TcpConnection>& conn,
    TlsConnection& tls,
    const std::string& encryptedData) {
    if (!conn) {
        return "";
    }

    if (!feed_tls_ciphertext(tls, encryptedData, conn->get_fd())) {
        return "";
    }

    if (tls.is_handshaking() && !advance_tls_handshake(tls, conn->get_fd())) {
        return "";
    }

    if (!flush_tls_output(conn, tls)) {
        return "";
    }

    if (tls.is_handshaking()) {
        return "";
    }

    if (!tls.is_established()) {
        spdlog::error("HttpServer: TlsConnection in error state for fd={}", conn->get_fd());
        return "";
    }

    return decrypt_tls_payload(tls, conn->get_fd());
}

std::string encrypt_tls_response(TlsConnection& tls, int fd, const std::string& plainData) {
    if (!tls.is_established()) {
        spdlog::error("HttpServer: Cannot encrypt, TLS not established for fd={}", fd);
        return "";
    }

    const int written = tls.encrypt(plainData.data(), plainData.size());
    if (written <= 0) {
        spdlog::error("HttpServer: TLS encrypt failed for fd={}", fd);
        return "";
    }

    return tls.get_output();
}

} // namespace

HttpServer::HttpServer(std::string ip, uint16_t port, int threadNum) :
    ip_(std::move(ip)),
    port_(port),
    tcpServer_(std::make_unique<TcpServer>(this->ip_, this->port_, threadNum)),
    connectionStates_(),
    contextsMutex_(),
    messageCallback_(nullptr),
    sslContext_(nullptr) {

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
    ConnectionState state = resolve_connection_state(conn);
    if (!state.httpContext) {
        return;
    }

    // 业务门面：统一呈现为“读入 -> 解析 -> 产出响应”的主干，TLS 细节留在文件内辅助函数中。
    const std::string payload = read_request_payload(conn, state);
    if (payload.empty()) {
        return;
    }

    const ParseState parseState = parse_http_request(*state.httpContext, payload);
    if (parseState == ParseState::NeedMoreData) {
        log_incomplete_request(conn->get_fd());
        return;
    }

    if (parseState == ParseState::Rejected) {
        reject_bad_request(conn, state, *state.httpContext);
        return;
    }

    reply_complete_request(conn, state, *state.httpContext);
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
    ConnectionState state = create_connection_state(fd);

    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        if (connectionStates_.find(fd) != connectionStates_.end()) {
            spdlog::warn("HttpServer: ConnectionState already exists for fd={}, overwriting.", fd);
        }
        connectionStates_[fd] = std::move(state);
    }

    spdlog::debug("HttpServer: New connection established, fd={}", fd);
}

HttpServer::ConnectionState HttpServer::create_connection_state(int fd) const {
    ConnectionState state;
    state.httpContext = std::make_shared<HttpContext>();

    if (!sslContext_) {
        return state;
    }

    SSL* ssl = sslContext_->create_ssl();
    if (!ssl) {
        spdlog::error("HttpServer: Failed to create SSL for fd={}", fd);
        return state;
    }

    spdlog::debug("HttpServer: TlsConnection created for fd={}", fd);
    state.tlsConnection = std::make_shared<TlsConnection>(ssl);
    return state;
}

void HttpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot close connection state, connection is nullptr");
        return;
    }

    const int fd = conn->get_fd();
    remove_connection_state(fd);
    spdlog::debug("HttpServer: Connection closed, fd={}", fd);
}

std::string HttpServer::read_request_payload(const std::shared_ptr<TcpConnection>& conn,
    const ConnectionState& state) const {
    if (!conn) {
        spdlog::error("HttpServer: Cannot receive data, connection is nullptr");
        return "";
    }

    std::string receivedData = conn->receive();
    if (receivedData.empty()) {
        return "";
    }

    if (state.tlsConnection) {
        return normalize_tls_payload(conn, *state.tlsConnection, receivedData);
    }

    if (is_ssl_enabled()) {
        spdlog::error("HttpServer: Missing TlsConnection for TLS-enabled server, fd={}", conn->get_fd());
        return "";
    }

    return normalize_plaintext_payload(std::move(receivedData));
}

HttpServer::ConnectionState HttpServer::find_connection_state(int fd) {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = connectionStates_.find(fd);
    if (it == connectionStates_.end()) {
        spdlog::error("HttpServer: No ConnectionState found for fd={}", fd);
        return ConnectionState();
    }

    return it->second;
}

HttpServer::ConnectionState HttpServer::resolve_connection_state(const std::shared_ptr<TcpConnection>& conn) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot resolve connection state, connection is nullptr");
        return ConnectionState();
    }

    return find_connection_state(conn->get_fd());
}

HttpServer::ParseState HttpServer::parse_http_request(HttpContext& ctx, const std::string& payload) const {
    size_t nparsed = 0;
    if (!ctx.parse(payload.data(), payload.size(), nparsed)) {
        return ParseState::Rejected;
    }

    // llhttp 接受了数据但消息未闭合时，需要保持上下文继续累计后续片段。
    if (!ctx.is_complete()) {
        return ParseState::NeedMoreData;
    }

    return ParseState::Complete;
}

void HttpServer::log_incomplete_request(int fd) const {
    spdlog::debug("HttpServer: HTTP request incomplete, waiting for more data, fd={}", fd);
}

void HttpServer::reject_bad_request(const std::shared_ptr<TcpConnection>& conn,
    const ConnectionState& state,
    HttpContext& ctx) {
    send_http_response(conn, state, build_bad_request_response());
    reset_http_context(ctx);
}

void HttpServer::reply_complete_request(const std::shared_ptr<TcpConnection>& conn,
    const ConnectionState& state,
    HttpContext& ctx) {
    send_http_response(conn, state, build_http_response(ctx.get_request()));
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
    return HttpResponse::plain_text(400, kBadRequestMessage, kBadRequestMessage);
}

HttpResponse HttpServer::build_not_found_response() const {
    return HttpResponse::plain_text(404, kNotFoundMessage, kNotFoundMessage);
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

void HttpServer::send_http_response(const std::shared_ptr<TcpConnection>& conn,
    const ConnectionState& state,
    HttpResponse resp) {
    // 响应在发送前统一补齐协议默认头，避免业务回调遗漏网络层必需字段。
    finalize_http_response(resp);
    send_response(conn, state, serialize_response(resp));
}

void HttpServer::send_response(const std::shared_ptr<TcpConnection>& conn,
    const ConnectionState& state,
    const std::string& response) {
    if (!conn) {
        spdlog::error("HttpServer: Cannot send data, connection is nullptr");
        return;
    }

    if (state.tlsConnection) {
        // HTTPS 场景下发送前统一加密，保持业务侧始终操作明文 HttpResponse。
        const std::string encrypted = encrypt_tls_response(*state.tlsConnection, conn->get_fd(), response);
        if (encrypted.empty()) {
            return;
        }

        conn->send(encrypted);
        return;
    }

    if (is_ssl_enabled()) {
        spdlog::error("HttpServer: Missing TlsConnection for TLS-enabled server, fd={}", conn->get_fd());
        return;
    }

    conn->send(response);
}

void HttpServer::reset_http_context(HttpContext& ctx) const {
    ctx.reset();
}

void HttpServer::remove_connection_state(int fd) {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = connectionStates_.find(fd);
    if (it == connectionStates_.end()) {
        spdlog::warn("HttpServer: No ConnectionState found for fd={} on close.", fd);
        return;
    }

    connectionStates_.erase(it);
}

