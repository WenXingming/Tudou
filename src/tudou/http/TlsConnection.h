// ============================================== //
// TlsConnection.h
// 单连接 TLS 会话门面，把握手、解密、加密压平成独立原子步骤。
// ============================================== //

#pragma once
#include <string>

// TlsConnection 只负责单连接 TLS 状态机，不参与任何 HTTP 业务编排。

// 前向声明
typedef struct ssl_st SSL;
typedef struct bio_st BIO;

class TlsConnection {
public:
    enum class State {
        HANDSHAKING,    // TLS 握手进行中
        ESTABLISHED,    // TLS 握手完成，可以传输数据
        ERROR           // 发生错误
    };

    /**
     * @brief 构造函数，接管 SSL 对象的所有权，创建 Memory BIO 对
     * @param ssl 由 SslContext::create_ssl() 创建的 SSL 对象，构造后由本对象负责释放
     */
    explicit TlsConnection(SSL* ssl);
    ~TlsConnection();

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    /**
     * @brief 将从网络收到的密文数据喂入 SSL 的读 BIO
     * @param data 密文数据
     * @param len 数据长度
     * @return 实际写入的字节数，-1 表示错误
     */
    int feed_data(const char* data, size_t len);

    /**
     * @brief 执行 TLS 握手（在 HANDSHAKING 状态下调用）
     *
     * 调用前应先通过 feed_data() 喂入客户端发来的握手数据。
     * 握手产生的响应数据需通过 get_output() 取出并发回客户端。
     *
     * @return true 握手完成或仍在进行中（需要更多数据），false 握手失败
     */
    bool do_handshake();

    /**
     * @brief 解密数据（在 ESTABLISHED 状态下调用）
     *
     * 调用前应先通过 feed_data() 喂入密文数据。
     *
     * @param plaintext 输出参数，解密后的明文数据会追加到此字符串
     * @return 解密得到的字节数，0 表示无数据可读，-1 表示错误
     */
    int decrypt(std::string& plaintext);

    /**
     * @brief 加密明文数据（在 ESTABLISHED 状态下调用）
     * @param data 明文数据
     * @param len 数据长度
     * @return 实际加密的字节数，-1 表示错误
     */
    int encrypt(const char* data, size_t len);

    /**
     * @brief 从 SSL 的写 BIO 中读取待发送的密文数据
     *
     * 握手响应和加密后的数据都通过此方法取出，然后由调用方通过 conn->send() 发送。
     *
     * @return 待发送的密文数据（可能为空）
     */
    std::string get_output();

    /**
     * @brief 获取当前 TLS 状态。
     * @return 当前连接的 TLS 状态枚举值。
     */
    State get_state() const { return state_; }

    /**
     * @brief 判断 TLS 是否仍在握手阶段。
     * @return true 表示需要继续交换握手数据。
     */
    bool is_handshaking() const { return state_ == State::HANDSHAKING; }

    /**
     * @brief 判断 TLS 是否已建立完成。
     * @return true 表示可以进行应用数据收发。
     */
    bool is_established() const { return state_ == State::ESTABLISHED; }

    /**
     * @brief 判断 TLS 是否已进入错误态。
     * @return true 表示当前连接已经不可恢复。
     */
    bool is_error() const { return state_ == State::ERROR; }

private:
    /**
     * @brief 初始化当前 TLS 会话需要的 Memory BIO 与服务端模式。
     */
    void initialize_tls_session();

    /**
     * @brief 创建当前会话的读写 Memory BIO。
     * @return 成功返回 true，失败返回 false。
     */
    bool create_memory_bio_pair();

    /**
     * @brief 把 Memory BIO 绑定到 SSL 会话。
     */
    void attach_memory_bio_pair();

    /**
     * @brief 检查当前 TLS 会话是否满足执行某个动作的前置条件。
     * @param action 当前准备执行的动作名称。
     * @return true 表示会话可继续执行。
     */
    bool ensure_tls_session(const char* action) const;

    /**
     * @brief 记录一个不可恢复的 TLS 错误并切换到 ERROR 状态。
     * @param message 错误描述。
     */
    void mark_error(const char* message);

    /**
     * @brief 根据 SSL_get_error 的返回值处理非阻塞握手结果。
     * @param action 当前动作名称。
     * @param result OpenSSL 原始返回值。
     * @return true 表示仍可继续推进，false 表示已进入错误态。
     */
    bool handle_tls_progress(const char* action, int result);

private:
    SSL* ssl_;      // SSL 对象，拥有所有权。
    BIO* rbio_;     // 读 BIO，承接来自网络的 TLS 密文。
    BIO* wbio_;     // 写 BIO，输出待发送的 TLS 密文。
    State state_;   // 当前 TLS 生命周期状态。
};
