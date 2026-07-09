// ============================================================================
// HttpServer.cpp
// HTTP/HTTPS 服务门面，把连接事件拍平成读取、解析、执行业务、发送响应。
// ============================================================================

#include "tudou/http/HttpServer.h"
#include "tudou/http/HttpContext.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/TlsConfig.h"
#include "tudou/http/TlsConnection.h"
#include "tudou/http/TlsProbe.h"

#include <algorithm>
#include <cerrno>
#include <unistd.h>

#include "spdlog/spdlog.h"
#include "tudou/tcp/TcpServer.h"

namespace {

constexpr char kContentLengthHeader[] = "Content-Length";
constexpr char kBadRequestMessage[] = "Bad Request";
constexpr size_t kTlsFileChunkSize = 16 * 1024;

} // namespace

HttpServer::HttpServer(std::string ip, uint16_t port, int threadNum) :
    ip_(std::move(ip)),
    port_(port),
    tcpServer_(std::make_unique<TcpServer>(this->ip_, this->port_, threadNum)),
    connectionStates_(),
    contextsMutex_(),
    router_(),
    tlsMode_(TlsMode::MemoryBio),
    tlsConfig_(nullptr) {

    bind_tcp_callbacks();
}

void HttpServer::start() {
    if (tlsConfig_ && tlsConfig_->is_initialized()) {
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

bool HttpServer::set_tls_mode(TlsMode mode) {
    if (mode == TlsMode::MemoryBio) {
        tlsMode_ = mode;
        return true;
    }

    if (mode == TlsMode::KernelTls) {
        if (TlsProbe::is_kernel_tls_supported()) {
            tlsMode_ = mode;
            if (tlsConfig_) {
                tlsConfig_->restrict_to_tls12();
            }
            return true;
        }
        spdlog::error("HttpServer: Kernel TLS is not supported on this platform/environment");
        return false;
    }

    spdlog::error("HttpServer: TlsMode::None is selected by not enabling SSL");
    return false;
}

bool HttpServer::enable_ssl(const std::string& certFile, const std::string& keyFile) {
    tlsConfig_ = std::make_unique<TlsConfig>();
    if (!tlsConfig_->init(certFile, keyFile)) {
        spdlog::critical("HttpServer: Failed to initialize TLS configuration");
        tlsConfig_.reset();
        return false;
    }

    if (tlsMode_ == TlsMode::KernelTls) {
        tlsConfig_->restrict_to_tls12();
    }

    spdlog::info("HttpServer: SSL enabled (cert={}, key={})", certFile, keyFile);
    return true;
}

bool HttpServer::is_ssl_enabled() const {
    return tlsConfig_ && tlsConfig_->is_initialized();
}

void HttpServer::on_message(const TcpConnectionPtr& conn) {
    const std::string receivedData = conn ? conn->receive() : std::string();
    if (receivedData.empty()) {
        return;
    }

    std::shared_ptr<ConnectionState> state = find_connection_state(conn);
    if (!state) {
        return;
    }

    // 1. 提取并归一化明文 payload（处理 TLS 解密与握手数据发回）
    std::string payload;
    if (state->tlsConnection && !state->isKtlsOffloaded) {
        std::string plaintext;
        std::string outboundCiphertext;
        const TlsConnection::ReadResult tlsResult =
            state->tlsConnection->read_plaintext(receivedData, plaintext, outboundCiphertext);

        // 如果解密/握手过程中产生了待发送的网络密文，立即发送出去
        if (!outboundCiphertext.empty()) {
            conn->send(outboundCiphertext);
        }

        if (tlsResult == TlsConnection::ReadResult::Error) {
            spdlog::error("HttpServer: TLS read failed for fd={}", conn ? conn->get_fd() : -1);
            return;
        }

        // 握手就绪时，若为 kTLS 模式，立刻尝试将 socket 卸载至内核
        if (state->tlsMode == TlsMode::KernelTls && state->tlsConnection->is_established()) {
            if (state->tlsConnection->enable_ktls_offload(conn->get_fd())) {
                state->isKtlsOffloaded = true;
            } else {
                spdlog::warn("HttpServer: kTLS offloading failed, falling back to Memory BIO, fd={}", conn ? conn->get_fd() : -1);
                state->tlsMode = TlsMode::MemoryBio;
            }
        }

        // 若 TLS 握手尚未完成或收到的是半包，静待下一波 TCP 可读事件
        if (tlsResult != TlsConnection::ReadResult::Ready && !state->isKtlsOffloaded) {
            return;
        }

        payload = std::move(plaintext);
    } else {
        // 安全校验：若服务器启用了 SSL 但并非 kTLS 卸载态，明文连接不应拥有缺失的 TlsConnection
        if (is_ssl_enabled() && !state->isKtlsOffloaded) {
            spdlog::error("HttpServer: Missing TlsConnection for TLS-enabled server, fd={}", conn ? conn->get_fd() : -1);
            return;
        }
        payload = receivedData;
    }

    // 2. 通过 while 循环逐个解析并消费粘包/管道化发送的 HTTP 请求，解决多请求丢弃漏洞。
    size_t consumed = 0;
    while (consumed < payload.size()) {
        const char* currentData = payload.data() + consumed;
        size_t currentLen = payload.size() - consumed;

        HttpContext::ParseResult result = state->httpContext.parse(currentData, currentLen);
        size_t lastConsumed = state->httpContext.get_consumed_bytes();
        consumed += lastConsumed;

        switch (result) {
        case HttpContext::ParseResult::NeedMoreData:
            spdlog::debug("HttpServer: HTTP request incomplete, waiting for more data, fd={}", conn ? conn->get_fd() : -1);
            break;
        case HttpContext::ParseResult::Rejected:
            // 直接就地回复 400 Bad Request，并重置当前连接的 HTTP 上下文
            send_http_response(conn, *state, HttpResponse::plain_text(400, kBadRequestMessage, kBadRequestMessage));
            state->httpContext.reset();
            return;
        case HttpContext::ParseResult::Complete:
            reply_complete_request(conn, *state);

            // 安全护栏：若响应中设置了 Connection: close 导致连接被 force_close() 关闭，
            // 对应的 ConnectionState 已经在 on_close 里被从 HttpServer 清理，必须退出防止野指针崩溃。
            if (!find_connection_state(conn)) {
                return;
            }
            break;
        }

        // 防死循环保护：如果未消费任何字节且未完成，直接跳出
        if (lastConsumed == 0 && result == HttpContext::ParseResult::NeedMoreData) {
            break;
        }
    }
}

void HttpServer::bind_tcp_callbacks() {
    // 事件回调只负责把 TcpServer 事件转发到 HTTP 门面，不再在 lambda 里编排业务细节。
    tcpServer_->set_connection_callback([this](const TcpConnectionPtr& conn) {
        on_connect(conn);
        });
    tcpServer_->set_message_callback([this](const TcpConnectionPtr& conn) {
        on_message(conn);
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

    if (!tlsConfig_) {
        return state;
    }

    SSL* ssl = tlsConfig_->create_ssl();
    if (!ssl) {
        spdlog::error("HttpServer: Failed to create SSL for fd={}", conn ? conn->get_fd() : -1);
        return state;
    }

    spdlog::debug("HttpServer: TlsConnection created for fd={}", conn ? conn->get_fd() : -1);
    state->tlsMode = tlsMode_;
    state->tlsConnection = std::make_unique<TlsConnection>(ssl);
    return state;
}

void HttpServer::on_close(const TcpConnectionPtr& conn) {
    remove_connection_state(conn);
    spdlog::debug("HttpServer: Connection closed, fd={}", conn ? conn->get_fd() : -1);
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

void HttpServer::send_http_response(const TcpConnectionPtr& conn,
    const ConnectionState& state,
    HttpResponse resp) {

    // 1. Content-Length 是网络契约的一部分，统一在基础设施层补齐，避免业务回调重复关注协议细节。
    if (!resp.has_header(kContentLengthHeader)) {
        const size_t bodySize = resp.has_file_body() ? resp.get_file_size() : resp.get_body().size();
        resp.set_header(kContentLengthHeader, std::to_string(bodySize));
    }

    // 2. 序列化 DTO 状态转换为完整协议报文
    std::string response = resp.package_to_string();

    // 3. 执行发送。TLS 模式显式分发，避免后续 kTLS 与 Memory BIO 逻辑混在一起。
    switch (tls_mode_of(state)) {
    case TlsMode::None:
        if (is_ssl_enabled()) {
            spdlog::error("HttpServer: Missing TlsConnection for TLS-enabled server, fd={}", conn ? conn->get_fd() : -1);
            return;
        }
        send_plain_response(conn, resp, response);
        break;
    case TlsMode::MemoryBio:
        if (!send_memory_bio_response(conn, *state.tlsConnection, resp, response)) {
            spdlog::error("HttpServer: Memory BIO TLS response failed, fd={}", conn ? conn->get_fd() : -1);
            return;
        }
        break;
    case TlsMode::KernelTls:
        if (state.isKtlsOffloaded) {
            // kTLS 已经在内核层接管了加密，我们可以直接以明文方式发送响应报文和文件
            send_plain_response(conn, resp, response);
        } else {
            if (!send_kernel_tls_response(conn, resp, response)) {
                spdlog::error("HttpServer: Kernel TLS response failed, fd={}", conn ? conn->get_fd() : -1);
                return;
            }
        }
        break;
    }

    // 4. 解决 Connection: close 连接泄漏漏洞（局限性：对于极其巨大的响应，若内核发送缓冲满导致未完全发送，调用 force_close() 可能会阶段性截断数据）
    if (resp.get_close_connection()) {
        conn->force_close();
    }
}

TlsMode HttpServer::tls_mode_of(const ConnectionState& state) const {
    return state.tlsMode;
}

void HttpServer::send_plain_response(const TcpConnectionPtr& conn,
    const HttpResponse& resp,
    const std::string& responseHead) {
    if (!conn) {
        return;
    }

    if (!resp.has_file_body()) {
        conn->send(responseHead);
        return;
    }

    const HttpResponse::FileBody& fileBody = resp.get_file_body();
    conn->send_file_with_header(responseHead, fileBody.file, fileBody.size, fileBody.offset);
}

bool HttpServer::send_memory_bio_response(const TcpConnectionPtr& conn,
    TlsConnection& tlsConnection,
    const HttpResponse& resp,
    const std::string& responseHead) {
    if (resp.has_file_body()) {
        return send_memory_bio_file_response(conn, tlsConnection, resp, responseHead);
    }

    return send_memory_bio_plaintext(conn, tlsConnection, responseHead);
}

bool HttpServer::send_memory_bio_plaintext(const TcpConnectionPtr& conn,
    TlsConnection& tlsConnection,
    const std::string& plaintext) {
    if (!conn) {
        return false;
    }

    std::string encrypted;
    if (!tlsConnection.write_plaintext(plaintext, encrypted)) {
        return false;
    }

    if (!encrypted.empty()) {
        conn->send(encrypted);
    }
    return true;
}

bool HttpServer::send_memory_bio_file_response(const TcpConnectionPtr& conn,
    TlsConnection& tlsConnection,
    const HttpResponse& resp,
    const std::string& responseHead) {
    if (!send_memory_bio_plaintext(conn, tlsConnection, responseHead)) {
        return false;
    }

    const HttpResponse::FileBody& fileBody = resp.get_file_body();
    size_t offset = fileBody.offset;
    size_t remaining = fileBody.size;

    while (remaining > 0) {
        std::string chunk(std::min(kTlsFileChunkSize, remaining), '\0');
        const ssize_t n = ::pread(fileBody.file->fd(), &chunk[0], chunk.size(), static_cast<off_t>(offset));
        if (n <= 0) {
            spdlog::error("HttpServer: failed to read Memory BIO TLS file body, fd={}, errno={}", fileBody.file->fd(), errno);
            return false;
        }

        chunk.resize(static_cast<size_t>(n));
        if (!send_memory_bio_plaintext(conn, tlsConnection, chunk)) {
            return false;
        }

        offset += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }

    return true;
}

bool HttpServer::send_kernel_tls_response(const TcpConnectionPtr& conn,
    const HttpResponse& resp,
    const std::string& responseHead) {
    (void)conn;
    (void)resp;
    (void)responseHead;
    spdlog::error("HttpServer: Kernel TLS is not supported yet");
    return false;
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
