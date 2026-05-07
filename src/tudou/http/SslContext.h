// ============================================================================
// SslContext.h
// TLS 服务端上下文门面，负责加载证书、校验私钥并按需创建连接级 SSL。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// SslContext.h
// └── SslContext
//     ├── SslContext()                           # [公有] 构造一个空 SSL_CTX 门面
//     ├── SslContext(copy)                       # [公有] 删除拷贝构造，禁止复制 SSL_CTX 所有权
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值，保持上下文唯一持有
//     ├── ~SslContext()                          # [公有] 析构时释放 SSL_CTX
//     │   └── reset_context()                    # [私有] 回收当前 SSL_CTX
//     ├── init(certFile, keyFile)                # [公有] 初始化总入口：创建 -> 配置 -> 加载证书/私钥 -> 校验
//     │   ├── reset_context()                    # [私有] 清理旧上下文
//     │   ├── create_server_context()            # [私有] 创建服务端 SSL_CTX
//     │   ├── configure_protocol_policy()        # [私有] 设置最小 TLS 版本等协议策略
//     │   ├── load_certificate(certFile)         # [私有] 加载服务端证书
//     │   ├── load_private_key(keyFile)          # [私有] 加载服务端私钥
//     │   └── validate_private_key()             # [私有] 校验证书与私钥匹配
//     ├── create_ssl() const                     # [公有] 为单连接创建 SSL 会话对象
//     └── is_initialized() const                 # [公有] 判断上下文是否已就绪
// ============================================================================

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

    bool init(const std::string& certFile, const std::string& keyFile); // 初始化 SSL_CTX 并加载证书/私钥。
    SSL* create_ssl() const; // 为一个新连接创建 SSL 会话对象。

    bool is_initialized() const { return ctx_ != nullptr; }

private:
    void reset_context();
    bool create_server_context();
    void configure_protocol_policy();
    bool load_certificate(const std::string& certFile);
    bool load_private_key(const std::string& keyFile);
    bool validate_private_key();

private:
    SSL_CTX* ctx_;  // 全局 TLS 服务端上下文，由 HttpServer 共享使用。
};
