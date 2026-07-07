#include "tudou/http/TlsConfig.h"
#include "spdlog/spdlog.h"

#include <openssl/ssl.h>

TlsConfig::TlsConfig() : ctx_(nullptr) {}

TlsConfig::~TlsConfig() {
    reset_context();
}

bool TlsConfig::init(const std::string& certFile, const std::string& keyFile) {
    // init 是唯一编排入口；每次重建都先清空旧上下文，避免失败重试时遗留旧状态。
    reset_context();

    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        spdlog::critical("TlsConfig: Failed to create SSL_CTX");
        return false;
    }

    // TLS 1.2 是当前服务端的最低安全线，统一在上下文层配置。
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(ctx_, certFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        spdlog::critical("TlsConfig: Failed to load certificate file: {}", certFile);
        reset_context();
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx_, keyFile.c_str(), SSL_FILETYPE_PEM) <= 0) {
        spdlog::critical("TlsConfig: Failed to load private key file: {}", keyFile);
        reset_context();
        return false;
    }

    if (!SSL_CTX_check_private_key(ctx_)) {
        spdlog::critical("TlsConfig: Private key does not match certificate");
        reset_context();
        return false;
    }

    spdlog::info("TlsConfig: TLS configuration initialized successfully (cert={}, key={})", certFile, keyFile);
    return true;
}

SSL* TlsConfig::create_ssl() const {
    if (!ctx_) {
        spdlog::error("TlsConfig: Cannot create SSL, context not initialized");
        return nullptr;
    }

    // 每个连接都从共享 SSL_CTX 派生一个独立 SSL 对象，后续握手和加解密状态都放在连接侧维护。
    SSL* ssl = SSL_new(ctx_);
    if (!ssl) {
        spdlog::error("TlsConfig: Failed to create SSL object");
    }
    return ssl;
}

void TlsConfig::reset_context() {
    if (!ctx_) {
        return;
    }

    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
}
