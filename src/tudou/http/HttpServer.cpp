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

namespace {

constexpr char kContentLengthHeader[] = "Content-Length";
constexpr char kBadRequestMessage[] = "Bad Request";

} // namespace

HttpServer::HttpServer(std::string ip, uint16_t port, int threadNum) :
    ip_(std::move(ip)),
    port_(port),
    tcpServer_(std::make_unique<TcpServer>(this->ip_, this->port_, threadNum)),
    connectionStates_(),
    contextsMutex_(),
    router_(),
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

void HttpServer::set_not_found_handler(Handler handler) {
    router_.set_not_found_handler(std::move(handler));
}

void HttpServer::set_method_not_allowed_handler(Handler handler) {
    router_.set_method_not_allowed_handler(std::move(handler));
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

void HttpServer::on_message(const TcpConnectionPtr& conn, const std::string& receivedData) {
    std::shared_ptr<ConnectionState> state = find_connection_state(conn);
    if (!state) {
        return;
    }

    std::string payload;
    if (!read_request_payload(conn, receivedData, *state, payload)) {
        return;
    }

    // 主线只保留“收数据 -> 解析 -> 分支响应”三个步骤，避免 TLS/状态仓库细节打断阅读。
    switch (state->httpContext.parse(payload.data(), payload.size())) {
    case HttpContext::ParseResult::NeedMoreData:
        log_incomplete_request(conn);
        return;
    case HttpContext::ParseResult::Rejected:
        reject_bad_request(conn, *state);
        return;
    case HttpContext::ParseResult::Complete:
        reply_complete_request(conn, *state);
        return;
    }
}

void HttpServer::bind_tcp_callbacks() {
    // 事件回调只负责把 TcpServer 事件转发到 HTTP 门面，不再在 lambda 里编排业务细节。
    tcpServer_->set_connection_callback([this](const TcpConnectionPtr& conn) {
        on_connect(conn);
        });
    tcpServer_->set_message_callback([this](const TcpConnectionPtr& conn, const std::string& receivedData) {
        on_message(conn, receivedData);
        });
    tcpServer_->set_close_callback([this](const TcpConnectionPtr& conn) {
        on_close(conn);
        });
}

void HttpServer::on_connect(const TcpConnectionPtr& conn) {
    std::shared_ptr<ConnectionState> state = create_connection_state(conn);

    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        if (connectionStates_.find(conn.get()) != connectionStates_.end()) {
            spdlog::warn("HttpServer: ConnectionState already exists for fd={}, overwriting.", conn ? conn->get_fd() : -1);
        }
        connectionStates_[conn.get()] = std::move(state);
    }

    spdlog::debug("HttpServer: New connection established, fd={}", conn ? conn->get_fd() : -1);
}

std::shared_ptr<HttpServer::ConnectionState> HttpServer::create_connection_state(const TcpConnectionPtr& conn) const {
    auto state = std::make_shared<ConnectionState>();

    if (!sslContext_) {
        return state;
    }

    SSL* ssl = sslContext_->create_ssl();
    if (!ssl) {
        spdlog::error("HttpServer: Failed to create SSL for fd={}", conn ? conn->get_fd() : -1);
        return state;
    }

    spdlog::debug("HttpServer: TlsConnection created for fd={}", conn ? conn->get_fd() : -1);
    state->tlsConnection = std::make_unique<TlsConnection>(ssl);
    return state;
}

void HttpServer::on_close(const TcpConnectionPtr& conn) {
    remove_connection_state(conn);
    spdlog::debug("HttpServer: Connection closed, fd={}", conn ? conn->get_fd() : -1);
}

bool HttpServer::read_request_payload(const TcpConnectionPtr& conn,
    const std::string& receivedData,
    ConnectionState& state,
    std::string& payload) const {
    payload.clear();

    if (receivedData.empty()) {
        return false;
    }

    if (state.tlsConnection) {
        std::string plaintext;
        std::string outboundCiphertext;
        const TlsConnection::ReadResult tlsResult =
            state.tlsConnection->read_plaintext(receivedData, plaintext, outboundCiphertext);

        if (!outboundCiphertext.empty()) {
            conn->send(outboundCiphertext);
        }

        if (tlsResult == TlsConnection::ReadResult::Error) {
            spdlog::error("HttpServer: TLS read failed for fd={}", conn ? conn->get_fd() : -1);
            return false;
        }

        if (tlsResult != TlsConnection::ReadResult::Ready) {
            return false;
        }

        payload = std::move(plaintext);
        return true;
    }

    if (is_ssl_enabled()) {
        spdlog::error("HttpServer: Missing TlsConnection for TLS-enabled server, fd={}", conn ? conn->get_fd() : -1);
        return false;
    }

    payload = receivedData;
    return true;
}

std::shared_ptr<HttpServer::ConnectionState> HttpServer::find_connection_state(const TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = connectionStates_.find(conn.get());
    if (it == connectionStates_.end()) {
        spdlog::error("HttpServer: No ConnectionState found for fd={}", conn ? conn->get_fd() : -1);
        return nullptr;
    }

    return it->second;
}

void HttpServer::log_incomplete_request(const TcpConnectionPtr& conn) const {
    spdlog::debug("HttpServer: HTTP request incomplete, waiting for more data, fd={}", conn ? conn->get_fd() : -1);
}

void HttpServer::reject_bad_request(const TcpConnectionPtr& conn,
    ConnectionState& state) {
    send_http_response(conn, state, build_bad_request_response());
    state.httpContext.reset();
}

void HttpServer::reply_complete_request(const TcpConnectionPtr& conn,
    ConnectionState& state) {
    send_http_response(conn, state, build_http_response(state.httpContext.get_request()));
    state.httpContext.reset();
}

HttpResponse HttpServer::build_http_response(const HttpRequest& req) const {
    HttpResponse response;
    // 路由分发与默认 404/405 统一收口在 HttpServer 内部，应用层只负责注册 handler。
    (void)router_.dispatch(req, response);
    return response;
}

HttpResponse HttpServer::build_bad_request_response() const {
    return HttpResponse::plain_text(400, kBadRequestMessage, kBadRequestMessage);
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

void HttpServer::send_http_response(const TcpConnectionPtr& conn,
    const ConnectionState& state,
    HttpResponse resp) {
    // 响应在发送前统一补齐协议默认头，避免业务回调遗漏网络层必需字段。
    finalize_http_response(resp);
    send_response(conn, state, serialize_response(resp));
}

void HttpServer::send_response(const TcpConnectionPtr& conn,
    const ConnectionState& state,
    const std::string& response) {
    if (state.tlsConnection) {
        // HTTPS 场景下发送前统一交给 TlsConnection 编码，保持业务侧始终操作明文 HttpResponse。
        std::string encrypted;
        if (!state.tlsConnection->write_plaintext(response, encrypted)) {
            spdlog::error("HttpServer: TLS write failed for fd={}", conn ? conn->get_fd() : -1);
            return;
        }

        if (!encrypted.empty()) {
            conn->send(encrypted);
        }
        return;
    }

    if (is_ssl_enabled()) {
        spdlog::error("HttpServer: Missing TlsConnection for TLS-enabled server, fd={}", conn ? conn->get_fd() : -1);
        return;
    }

    conn->send(response);
}

void HttpServer::remove_connection_state(const TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = connectionStates_.find(conn.get());
    if (it == connectionStates_.end()) {
        spdlog::warn("HttpServer: No ConnectionState found for fd={} on close.", conn ? conn->get_fd() : -1);
        return;
    }

    connectionStates_.erase(it);
}
