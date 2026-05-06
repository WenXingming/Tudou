// ============================================== //
// SslContext.h
// TLS 服务端上下文门面，负责加载证书、校验私钥并按需创建连接级 SSL。
// ============================================== //

#pragma once
#include <string>

// SslContext 把 SSL_CTX 的初始化收敛成单向步骤，避免调用方散落处理证书和失败清理。

// 前向声明 OpenSSL 类型，避免头文件污染
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

class SslContext {
public:
    SslContext();
    ~SslContext();

    SslContext(const SslContext&) = delete;
    SslContext& operator=(const SslContext&) = delete;

    /**
     * @brief 初始化 SSL 上下文，加载证书和私钥
     * @param certFile 证书文件路径（PEM 格式）
     * @param keyFile 私钥文件路径（PEM 格式）
     * @return 成功返回 true，失败返回 false
     */
    bool init(const std::string& certFile, const std::string& keyFile);

    /**
     * @brief 为新连接创建一个 SSL 对象
     * @return SSL* 成功返回 SSL 指针，失败返回 nullptr。调用方负责释放（通过 SSL_free）
     */
    SSL* create_ssl() const;

    /**
     * @brief 判断当前上下文是否已经完成初始化。
     * @return true 表示证书、私钥和协议策略都已就绪。
     */
    bool is_initialized() const { return ctx_ != nullptr; }

private:
    /**
     * @brief 释放当前 SSL_CTX 并回到未初始化状态。
     */
    void reset_context();

    /**
     * @brief 创建服务端 SSL_CTX。
     * @return 成功返回 true，失败返回 false。
     */
    bool create_server_context();

    /**
     * @brief 配置服务端最小 TLS 版本等基础协议策略。
     */
    void configure_protocol_policy();

    /**
     * @brief 加载服务端证书。
     * @param certFile PEM 格式证书路径。
     * @return 成功返回 true，失败返回 false。
     */
    bool load_certificate(const std::string& certFile);

    /**
     * @brief 加载服务端私钥。
     * @param keyFile PEM 格式私钥路径。
     * @return 成功返回 true，失败返回 false。
     */
    bool load_private_key(const std::string& keyFile);

    /**
     * @brief 校验证书与私钥是否匹配。
     * @return 成功返回 true，失败返回 false。
     */
    bool validate_private_key();

private:
    SSL_CTX* ctx_;  // 全局 TLS 服务端上下文，由 HttpServer 共享使用。
};
