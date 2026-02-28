/**
 * @file SslContext.h
 * @brief SSL/TLS 上下文封装类，管理全局 SSL_CTX 以及证书/私钥加载
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 * SslContext 封装 OpenSSL 的 SSL_CTX，提供：
 *   - 初始化 SSL 库
 *   - 加载服务器证书文件和私钥文件
 *   - 为每个新连接创建 SSL 对象（配合 Memory BIO 使用）
 *
 * 整个 HttpServer 共享一个 SslContext 实例。
 */

#pragma once
#include <string>

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

    bool is_initialized() const { return ctx_ != nullptr; }

private:
    SSL_CTX* ctx_;
};
