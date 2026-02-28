/**
 * @file SslContext.cpp
 * @brief SSL/TLS 上下文封装实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "SslContext.h"
#include "spdlog/spdlog.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

SslContext::SslContext() : ctx_(nullptr) {}

SslContext::~SslContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

bool SslContext::init(const std::string& certFile, const std::string& keyFile) {
    // 创建 SSL 上下文（使用 TLS 服务端方法，自动协商最高版本）
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        spdlog::critical("SslContext: Failed to create SSL_CTX");
        return false;
    }

    // 设置最低 TLS 版本为 1.2（禁用已知不安全的 TLS 1.0/1.1）
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

    // 加载服务器证书
    if (SSL_CTX_use_certificate_file(ctx_, certFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        spdlog::critical("SslContext: Failed to load certificate file: {}", certFile);
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return false;
    }

    // 加载服务器私钥
    if (SSL_CTX_use_PrivateKey_file(ctx_, keyFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        spdlog::critical("SslContext: Failed to load private key file: {}", keyFile);
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return false;
    }

    // 验证私钥与证书匹配
    if (!SSL_CTX_check_private_key(ctx_)) {
        spdlog::critical("SslContext: Private key does not match certificate");
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return false;
    }

    spdlog::info("SslContext: SSL context initialized successfully (cert={}, key={})", certFile, keyFile);
    return true;
}

SSL* SslContext::create_ssl() const {
    if (!ctx_) {
        spdlog::error("SslContext: Cannot create SSL, context not initialized");
        return nullptr;
    }
    SSL* ssl = SSL_new(ctx_);
    if (!ssl) {
        spdlog::error("SslContext: Failed to create SSL object");
    }
    return ssl;
}
