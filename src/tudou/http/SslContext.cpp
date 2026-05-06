// ============================================== //
// SslContext.cpp
// TLS 服务端上下文实现，线性执行“创建 -> 配置 -> 加载证书 -> 加载私钥 -> 校验契约”。
// ============================================== //

#include "SslContext.h"
#include "spdlog/spdlog.h"

#include <openssl/ssl.h>

SslContext::SslContext() : ctx_(nullptr) {}

SslContext::~SslContext() {
    reset_context();
}

bool SslContext::init(const std::string& certFile, const std::string& keyFile) {
    // init 是唯一编排入口；每次重建都先清空旧上下文，避免失败重试时遗留旧状态。
    reset_context();
    if (!create_server_context()) {
        return false;
    }

    configure_protocol_policy();
    if (!load_certificate(certFile)) {
        reset_context();
        return false;
    }

    if (!load_private_key(keyFile)) {
        reset_context();
        return false;
    }

    if (!validate_private_key()) {
        reset_context();
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

void SslContext::reset_context() {
    if (!ctx_) {
        return;
    }

    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
}

bool SslContext::create_server_context() {
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        spdlog::critical("SslContext: Failed to create SSL_CTX");
        return false;
    }

    return true;
}

void SslContext::configure_protocol_policy() {
    // TLS 1.2 是当前服务端的最低安全线，统一在上下文层收口而不是分散到连接层。
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
}

bool SslContext::load_certificate(const std::string& certFile) {
    if (SSL_CTX_use_certificate_file(ctx_, certFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        spdlog::critical("SslContext: Failed to load certificate file: {}", certFile);
        return false;
    }

    return true;
}

bool SslContext::load_private_key(const std::string& keyFile) {
    if (SSL_CTX_use_PrivateKey_file(ctx_, keyFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        spdlog::critical("SslContext: Failed to load private key file: {}", keyFile);
        return false;
    }

    return true;
}

bool SslContext::validate_private_key() {
    if (!SSL_CTX_check_private_key(ctx_)) {
        spdlog::critical("SslContext: Private key does not match certificate");
        return false;
    }

    return true;
}
