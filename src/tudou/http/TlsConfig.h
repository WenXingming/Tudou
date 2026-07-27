// ============================================================================
// 服务端 TLS 配置：持有共享 SSL_CTX，加载证书、私钥和协议策略，并创建 SSL 连接会话。
// 由 HttpServer 在启动前初始化；不负责单连接握手、加解密或 Socket I/O。
// ============================================================================

#pragma once

#include <string>

#include <openssl/ssl.h>

class TlsConfig {
public:
    TlsConfig();
    ~TlsConfig();

    TlsConfig(const TlsConfig&) = delete;
    TlsConfig& operator=(const TlsConfig&) = delete;

    // 重建服务端上下文；失败后对象处于未初始化状态。
    bool init(const std::string& certFile, const std::string& keyFile);

    // 创建独立的连接会话；调用者负责 SSL_free()。
    SSL* create_ssl_session() const;

    bool is_initialized() const { return ctx_ != nullptr; }

private:
    // 释放当前服务端上下文，使对象回到未初始化状态。
    void reset_context();

private:
    SSL_CTX* ctx_; // 由 HttpServer 共享的服务端 TLS 上下文。
};
