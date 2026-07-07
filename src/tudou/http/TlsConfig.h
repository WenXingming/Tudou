#pragma once
#include <string>

// TlsConfig 把 SSL_CTX 的初始化收敛成单向步骤，作为全局共享的安全传输配置工厂。

// 前向声明 OpenSSL 类型，避免头文件污染
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

class TlsConfig {
public:
    TlsConfig();
    ~TlsConfig();

    TlsConfig(const TlsConfig&) = delete;
    TlsConfig& operator=(const TlsConfig&) = delete;

    bool init(const std::string& certFile, const std::string& keyFile); // 初始化 SSL_CTX 并加载证书/私钥。
    SSL* create_ssl() const; // 为一个新连接创建 SSL 会话对象。

    bool is_initialized() const { return ctx_ != nullptr; }

private:
    void reset_context();

private:
    SSL_CTX* ctx_;                      // 全局 TLS 服务端上下文，由 HttpServer 共享使用。
};
