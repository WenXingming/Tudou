// ============================================================================
// TlsConnection.h
// 单连接 TLS 会话门面，把握手、解密、加密压平成独立原子步骤。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// TlsConnection.h
// └── TlsConnection
//     ├── TlsConnection(ssl)                     # [公有] 接管 SSL 并初始化 Memory BIO + 服务端模式
//     │   └── initialize_tls_session()           # [私有] 建立单连接 TLS 会话基础设施
//     │       ├── create_memory_bio_pair()       # [私有] 创建读写 Memory BIO
//     │       └── attach_memory_bio_pair()       # [私有] 将 BIO 绑定到 SSL
//     ├── TlsConnection(copy)                    # [公有] 删除拷贝构造，避免复制连接级 TLS 状态
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值，保持 SSL/BIO 唯一所有权
//     ├── ~TlsConnection()                       # [公有] 释放 SSL，对应 BIO 由 SSL 一并回收
//     ├── read_plaintext(ciphertext, plaintext, outboundCiphertext) # [公有] 喂入密文并推进握手/解密
//     │   ├── feed_ciphertext(data, len)         # [私有] 把网络密文写入读 BIO
//     │   │   ├── ensure_tls_session(action) const   # [私有] 校验会话是否可继续执行
//     │   │   └── mark_error(message)            # [私有] BIO_write 失败时切换 ERROR 状态
//     │   ├── advance_handshake()                # [私有] 推进一次 TLS 握手
//     │   │   ├── ensure_tls_session(action) const   # [私有] 校验会话状态
//     │   │   └── handle_tls_progress(action, result) # [私有] 处理 WANT_READ/WANT_WRITE 或错误态
//     │   ├── drain_ciphertext()                 # [私有] 从写 BIO 取出待发送密文
//     │   └── drain_plaintext(plaintext)         # [私有] 从 SSL 会话中持续读出明文
//     │       ├── ensure_tls_session(action) const   # [私有] 校验会话状态
//     │       └── mark_error(message)            # [私有] SSL_read 致命失败时切 ERROR
//     ├── write_plaintext(plaintext, ciphertext) # [公有] 把明文写入 SSL 并取出待发送密文
//     │   ├── seal_plaintext(data, len)          # [私有] 把明文写入 SSL
//     │   │   ├── ensure_tls_session(action) const   # [私有] 校验会话状态
//     │   │   └── mark_error(message)            # [私有] SSL_write 致命失败时切 ERROR
//     │   └── drain_ciphertext()                 # [私有] 从写 BIO 取出待发送密文
//     ├── get_state() const                      # [公有] 读取 TLS 生命周期状态
//     ├── is_handshaking() const                 # [公有] 判断是否仍在握手阶段
//     ├── is_established() const                 # [公有] 判断是否已建立完成
//     └── is_error() const                       # [公有] 判断是否已进入错误态
// ============================================================================

#pragma once
#include <string>

// TlsConnection 只负责单连接 TLS 状态机，不参与任何 HTTP 业务编排。

// 前向声明
typedef struct ssl_st SSL;
typedef struct bio_st BIO;

class TlsConnection {
public:
    enum class ReadResult {
        Error,
        NeedMoreData,
        Ready
    };

    enum class State {
        HANDSHAKING,    // TLS 握手进行中
        ESTABLISHED,    // TLS 握手完成，可以传输数据
        ERROR           // 发生错误
    };

    explicit TlsConnection(SSL* ssl);
    ~TlsConnection();

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    ReadResult read_plaintext(
        const std::string& ciphertext,
        std::string& plaintext,
        std::string& outboundCiphertext); // 喂入密文并返回本轮可消费的明文与待发送密文。
    bool write_plaintext(const std::string& plaintext, std::string& ciphertext); // 把明文编码成可直接发送的 TLS 密文。

    State get_state() const { return state_; }
    bool is_handshaking() const { return state_ == State::HANDSHAKING; }
    bool is_established() const { return state_ == State::ESTABLISHED; }
    bool is_error() const { return state_ == State::ERROR; }

private:
    void initialize_tls_session();
    bool create_memory_bio_pair();
    void attach_memory_bio_pair();
    int feed_ciphertext(const char* data, size_t len);
    bool advance_handshake();
    int drain_plaintext(std::string& plaintext);
    int seal_plaintext(const char* data, size_t len);
    std::string drain_ciphertext();
    bool ensure_tls_session(const char* action) const; // 校验 SSL/BIO/状态机是否可继续执行。
    void mark_error(const char* message); // 记录不可恢复错误并切换 ERROR。
    bool handle_tls_progress(const char* action, int result);

private:
    SSL* ssl_;      // SSL 对象，拥有所有权。
    BIO* rbio_;     // 读 BIO，承接来自网络的 TLS 密文。
    BIO* wbio_;     // 写 BIO，输出待发送的 TLS 密文。
    State state_;   // 当前 TLS 生命周期状态。
};
