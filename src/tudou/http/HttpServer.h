// ============================================== //
// HttpServer.h
// HTTP/HTTPS 服务器门面，把传输层事件压平成“读取、解析、执行业务、发送响应”。
// ============================================== //

#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "tudou/tcp/TcpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/HttpContext.h"
#include "tudou/http/SslContext.h"
#include "tudou/http/TlsConnection.h"

// HttpServer 是 HTTP/HTTPS 协议门面，负责把连接事件翻译成稳定的请求-响应流程。
class HttpServer {
    using MessageCallback = std::function<void(const HttpRequest& req, HttpResponse& resp)>;

public:
    HttpServer(std::string ip, uint16_t port, int threadNum = 0);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer() = default;

    /**
     * @brief 启动底层 TCP 服务并开始接受连接。
     */
    void start();

    /**
     * @brief 设置 HTTP 业务回调。
     * @param cb 收到完整 HttpRequest 后用于填充 HttpResponse 的业务函数。
     */
    void set_http_callback(const MessageCallback& cb);

    /**
     * @brief 启用 HTTPS 支持（必须在 start() 之前调用）
     * @param certFile 证书文件路径（PEM 格式）
     * @param keyFile 私钥文件路径（PEM 格式）
     * @return 成功返回 true，失败返回 false
     */
    bool enable_ssl(const std::string& certFile, const std::string& keyFile);

    /**
     * @brief 判断当前服务是否启用了 TLS。
     * @return true 表示 TLS 已启用且完成初始化。
     */
    bool is_ssl_enabled() const;

    /**
     * @brief 获取监听 IP。
     * @return 当前服务监听的 IP。
     */
    const std::string& get_ip() const;

    /**
     * @brief 获取监听端口。
     * @return 当前服务监听的端口。
     */
    int get_port() const;

    /**
     * @brief 获取底层 TCP 线程数。
     * @return 底层 TcpServer 的 IO 线程数。
     */
    int get_thread_num() const;

    /**
     * @brief 执行一次连接上的完整 HTTP 消息处理流程。
     * @param conn 触发消息事件的连接对象。
     */
    void process(const std::shared_ptr<TcpConnection>& conn);

private:
    struct TransportPayload {
        bool ready = false;
        std::string plaintext;
    };

    enum class ParseState {
        Rejected,
        NeedMoreData,
        Complete
    };

    /**
     * @brief 绑定底层 TcpServer 回调到当前 HTTP 门面。
     */
    void bind_tcp_callbacks();

    /**
     * @brief 处理新连接建立事件。
     * @param conn 新建立的 TCP 连接。
     */
    void on_connect(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 为新连接创建解析上下文。
     * @return 新建的 HttpContext。
     */
    std::shared_ptr<HttpContext> create_http_context() const;

    /**
     * @brief 为启用 TLS 的连接创建 TlsConnection。
     * @param fd 连接 fd，仅用于日志定位。
     * @return 创建成功时返回 TlsConnection；失败时返回空指针。
     */
    std::shared_ptr<TlsConnection> create_tls_connection(int fd) const;

    /**
     * @brief 处理连接关闭事件。
     * @param conn 已关闭的 TCP 连接。
     */
    void on_close(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 从连接读取本次收到的原始数据。
     * @param conn 触发消息事件的连接对象。
     * @return 本次收到的数据；连接为空时返回空字符串。
     */
    std::string receive_data(const std::shared_ptr<TcpConnection>& conn) const;

    /**
     * @brief 将传输层输入归一化成可供 HTTP 解析的明文。
     * @param conn 触发消息事件的连接对象。
     * @param receivedData 刚从连接读出的原始数据。
     * @return 若返回 ready=true，则 plaintext 可直接交给 HttpContext 解析。
     */
    TransportPayload normalize_transport_payload(const std::shared_ptr<TcpConnection>& conn,
                                                 std::string receivedData);

    /**
     * @brief 归一化明文 HTTP 输入。
     * @param receivedData 本次收到的明文数据。
     * @return 可直接解析的传输载荷。
     */
    TransportPayload normalize_plaintext_payload(std::string receivedData) const;

    /**
     * @brief 归一化 TLS 输入，负责喂入握手/解密流程并产出 HTTP 明文。
     * @param conn 触发消息事件的连接对象。
     * @param encryptedData 本次收到的 TLS 密文。
     * @return 若返回 ready=true，则 plaintext 为已解密的 HTTP 明文。
     */
    TransportPayload normalize_tls_payload(const std::shared_ptr<TcpConnection>& conn,
                                           const std::string& encryptedData);

    /**
     * @brief 将 TLS 密文写入当前连接的读 BIO。
     * @param tls 当前连接的 TLS 状态机。
     * @param encryptedData 本次收到的 TLS 密文。
     * @param fd 连接 fd，仅用于日志定位。
     * @return 成功返回 true，失败返回 false。
     */
    bool feed_tls_ciphertext(TlsConnection& tls, const std::string& encryptedData, int fd) const;

    /**
     * @brief 推进一次 TLS 握手。
     * @param tls 当前连接的 TLS 状态机。
     * @param fd 连接 fd，仅用于日志定位。
     * @return 成功返回 true，失败返回 false。
     */
    bool advance_tls_handshake(TlsConnection& tls, int fd) const;

    /**
     * @brief 把 TLS 写 BIO 中的密文回写到当前连接。
     * @param conn 当前 TCP 连接。
     * @param tls 当前连接的 TLS 状态机。
     * @return 成功返回 true，失败返回 false。
     */
    bool flush_tls_output(const std::shared_ptr<TcpConnection>& conn, TlsConnection& tls) const;

    /**
     * @brief 从已建立的 TLS 会话中解密出 HTTP 明文。
     * @param tls 当前连接的 TLS 状态机。
     * @param fd 连接 fd，仅用于日志定位。
     * @return 已解密的传输载荷；失败时返回 ready=false。
     */
    TransportPayload decrypt_tls_payload(TlsConnection& tls, int fd) const;

    /**
     * @brief 从连接状态表中获取对应的 HttpContext。
     * @param fd 连接 fd。
     * @return 找到则返回 HttpContext；否则返回空指针。
     */
    std::shared_ptr<HttpContext> find_http_context(int fd);

    /**
     * @brief 根据连接对象解析出当前请求上下文。
     * @param conn 当前连接。
     * @return 找到则返回 HttpContext；否则返回空指针。
     */
    std::shared_ptr<HttpContext> resolve_request_context(const std::shared_ptr<TcpConnection>& conn);

    /**
     * @brief 解析 HTTP 明文并返回当前解析阶段。
     * @param ctx 当前连接对应的 HttpContext。
     * @param payload 已归一化的 HTTP 明文。
     * @return 解析结果状态，决定后续是等待、拒绝还是执行业务逻辑。
     */
    ParseState parse_http_request(HttpContext& ctx, const std::string& payload) const;

    /**
     * @brief 记录请求仍未完整的调试日志。
     * @param fd 连接 fd。
     */
    void log_incomplete_request(int fd) const;

    /**
     * @brief 返回标准 400 响应并重置当前解析状态。
     * @param conn 当前连接。
     * @param ctx 当前连接对应的 HttpContext。
     */
    void reject_bad_request(const std::shared_ptr<TcpConnection>& conn, HttpContext& ctx);

    /**
     * @brief 基于完整请求执行业务回调并发送响应。
     * @param conn 当前连接。
     * @param ctx 当前连接对应的 HttpContext。
     */
    void reply_complete_request(const std::shared_ptr<TcpConnection>& conn, HttpContext& ctx);

    /**
     * @brief 根据业务回调构建响应对象。
     * @param req 完整解析出的 HTTP 请求。
     * @return 业务层填充后的响应；未设置回调时返回默认 404 响应。
     */
    HttpResponse build_http_response(const HttpRequest& req) const;

    /**
     * @brief 构建标准 400 Bad Request 响应。
     * @return 协议完整的 400 响应。
     */
    HttpResponse build_bad_request_response() const;

    /**
     * @brief 构建标准 404 Not Found 响应。
     * @return 协议完整的 404 响应。
     */
    HttpResponse build_not_found_response() const;

    /**
     * @brief 构建一个纯文本错误响应。
     * @param statusCode HTTP 状态码。
     * @param statusMessage HTTP 状态描述。
     * @param body 响应体内容。
     * @return 已补齐基础头部的错误响应。
     */
    HttpResponse build_plain_text_response(int statusCode,
                                           const std::string& statusMessage,
                                           const std::string& body) const;

    /**
     * @brief 补齐业务层未显式设置的协议头。
     * @param resp 待发送的响应对象。
     */
    void finalize_http_response(HttpResponse& resp) const;

    /**
     * @brief 将响应对象序列化为字符串。
     * @param resp 待发送的响应对象。
     * @return 可直接发往传输层的响应字符串。
     */
    std::string serialize_response(const HttpResponse& resp) const;

    /**
     * @brief 发送一个完整的 HTTP 响应对象。
     * @param conn 目标连接。
     * @param resp 待发送的响应对象。
     */
    void send_http_response(const std::shared_ptr<TcpConnection>& conn, HttpResponse resp);

    /**
     * @brief 把响应字符串发送到连接。
     * @param conn 目标连接。
     * @param response 已序列化的响应字符串。
     */
    void send_response(const std::shared_ptr<TcpConnection>& conn, const std::string& response);

    /**
     * @brief 将一个连接的 HTTP 解析状态重置到初始状态。
     * @param ctx 当前连接对应的 HttpContext。
     */
    void reset_http_context(HttpContext& ctx) const;

    /**
     * @brief 从连接状态表中获取 TLS 连接状态。
     * @param fd 连接 fd。
     * @return 找到则返回 TlsConnection；否则返回空指针。
     */
    std::shared_ptr<TlsConnection> find_tls_connection(int fd);

    /**
     * @brief 将明文响应加密成 TLS 密文。
     * @param fd 连接 fd。
     * @param plainData 待加密的明文。
     * @return 加密后的密文；失败时返回空字符串。
     */
    std::string encrypt_response(int fd, const std::string& plainData);

    /**
     * @brief 清理连接对应的 HttpContext。
     * @param fd 连接 fd。
     */
    void remove_http_context(int fd);

    /**
     * @brief 清理连接对应的 TlsConnection。
     * @param fd 连接 fd。
     */
    void remove_tls_connection(int fd);

private:
    std::string ip_;                              // 服务监听 IP。
    uint16_t port_;                               // 服务监听端口。
    std::unique_ptr<TcpServer> tcpServer_;        // 底层 TCP 服务器门面。

    std::unordered_map<int, std::shared_ptr<HttpContext>> httpContexts_; // 每个连接一个 HttpContext，隔离解析状态。
    std::mutex contextsMutex_;                                            // 保护 HTTP/TLS 连接状态映射。

    MessageCallback messageCallback_;             // 上层 HTTP 业务回调，负责把请求翻译成响应。

    std::unique_ptr<SslContext> sslContext_;                                  // 全局 SSL 上下文，持有证书与私钥。
    std::unordered_map<int, std::shared_ptr<TlsConnection>> tlsConnections_;  // 每个 TLS 连接一个握手/加解密状态。
};
