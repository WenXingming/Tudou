// ============================================================================
// 单连接 TLS 会话：通过 Memory BIO 在网络密文与 HTTP 明文之间转换。
// 拥有 SSL 及其 BIO；不负责 Socket I/O、HTTP 解析或连接生命周期。
// ============================================================================

#pragma once

#include <string>

#include <openssl/ssl.h>

class TlsConnection {
public:
    enum class ReadResult {
        Error,          // TLS 会话不可继续使用。
        NeedMoreData,   // 尚无 HTTP 明文，等待更多网络密文。
        Ready           // 已得到可交给 HTTP 层的明文。
    };

    explicit TlsConnection(SSL* ssl);
    ~TlsConnection();

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    // 输入网络密文，输出本轮解出的明文和握手产生的待发密文。outboundCiphertext 代表“由本次读操作触发的、必须反向发送给客户端的网络密文数据”，如握手阶段
    ReadResult read_plaintext(const std::string& ciphertext, std::string& plaintext, std::string& outboundCiphertext);

    // 将 HTTP 明文加密为可直接发送的网络密文；本轮无法完成时返回 false。
    bool write_plaintext(const std::string& plaintext, std::string& ciphertext);

    bool is_established() const { return ssl_ && SSL_is_init_finished(ssl_); }
    bool is_error() const { return ssl_ == nullptr; }

private:
    bool advance_handshake(); // 推进握手；WANT_READ/WANT_WRITE 时等待下一次网络事件。
    std::string drain_ciphertext(); // 取出写 BIO 中全部待发送密文。
    void mark_error(const char* message); // 记录错误并释放不可继续使用的 SSL。

private:
    SSL* ssl_; // 拥有 SSL；SSL 同时拥有读写 Memory BIO；空表示错误。
};
