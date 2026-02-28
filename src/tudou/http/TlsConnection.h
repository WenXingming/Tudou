/**
 * @file TlsConnection.h
 * @brief 每连接的 TLS 状态封装，基于 OpenSSL Memory BIO 实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 * TlsConnection 利用 OpenSSL 的 Memory BIO 机制，在不修改 TCP 层的前提下实现 TLS 加解密。
 *
 * 原理：
 *   普通 SSL 直接操作 socket fd，需要改动 TcpConnection。
 *   Memory BIO 模式下，SSL 读写的是内存缓冲区而非 fd，数据搬运由上层（HttpServer）控制：
 *
 *   接收方向：conn->receive() → 密文 → feed_data() → decrypt() → 明文 → HttpContext 解析
 *   发送方向：明文 → encrypt() → 密文 → get_output() → conn->send()
 *
 * 生命周期：
 *   每个 HTTPS 连接持有一个 TlsConnection，与 HttpContext 一一对应。
 *   由 HttpServer 在 on_connect 时创建，on_close 时销毁。
 *
 * TLS 握手：
 *   新连接建立时处于 HANDSHAKING 状态。HttpServer 收到数据后调用 do_handshake()，
 *   握手产生的响应数据通过 get_output() 取出并发回客户端。
 *   握手完成后进入 ESTABLISHED 状态，后续使用 decrypt()/encrypt() 处理数据。
 */

#pragma once
#include <string>

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

    State get_state() const { return state_; }
    bool is_handshaking() const { return state_ == State::HANDSHAKING; }
    bool is_established() const { return state_ == State::ESTABLISHED; }
    bool is_error() const { return state_ == State::ERROR; }

private:
    SSL* ssl_;      // SSL 对象，拥有所有权
    BIO* rbio_;     // 读 BIO（接收方向：网络密文 → rbio → SSL_read → 明文）
    BIO* wbio_;     // 写 BIO（发送方向：明文 → SSL_write → wbio → 网络密文）
    State state_;
};
