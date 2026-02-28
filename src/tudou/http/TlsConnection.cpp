/**
 * @file TlsConnection.cpp
 * @brief 每连接的 TLS 状态封装实现，基于 OpenSSL Memory BIO
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "TlsConnection.h"
#include "spdlog/spdlog.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

 // SSL_read/SSL_write 使用的临时缓冲区大小
static const int kTlsBufferSize = 16384;  // 16KB，与 TLS record 最大长度一致

TlsConnection::TlsConnection(SSL* ssl)
    : ssl_(ssl)
    , rbio_(nullptr)
    , wbio_(nullptr)
    , state_(State::HANDSHAKING) {

    // 创建 Memory BIO 对
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());

    // 将 BIO 绑定到 SSL，SSL_set_bio 会接管 BIO 的所有权
    // 之后 rbio_/wbio_ 指针仍然有效，但不需要单独释放（SSL_free 时会自动释放）
    SSL_set_bio(ssl_, rbio_, wbio_);

    // 设置为服务端模式，等待客户端发起握手
    SSL_set_accept_state(ssl_);
}

TlsConnection::~TlsConnection() {
    if (ssl_) {
        SSL_free(ssl_);  // 同时释放关联的 BIO
        ssl_ = nullptr;
    }
    // rbio_ 和 wbio_ 由 SSL_free 自动释放，不要重复释放
}

int TlsConnection::feed_data(const char* data, size_t len) {
    if (!data || len == 0) return 0;

    // 将密文写入读 BIO，供 SSL_read/SSL_do_handshake 消费
    int written = BIO_write(rbio_, data, static_cast<int>(len));
    if (written <= 0) {
        spdlog::error("TlsConnection: BIO_write failed");
        return -1;
    }
    return written;
}

bool TlsConnection::do_handshake() {
    int ret = SSL_do_handshake(ssl_);

    if (ret == 1) {
        // 握手完成
        state_ = State::ESTABLISHED;
        spdlog::debug("TlsConnection: TLS handshake completed successfully");
        return true;
    }

    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // 握手尚未完成，需要更多数据（正常流程，多轮握手）
        // 调用方应通过 get_output() 取出握手响应并发送，然后等待更多客户端数据
        return true;
    }

    // 握手失败
    state_ = State::ERROR;
    spdlog::error("TlsConnection: TLS handshake failed, SSL_get_error={}", err);
    return false;
}

int TlsConnection::decrypt(std::string& plaintext) {
    if (state_ != State::ESTABLISHED) {
        spdlog::warn("TlsConnection: Cannot decrypt, TLS not established");
        return -1;
    }

    char buf[kTlsBufferSize];
    int totalRead = 0;

    // 循环读取，因为一次 feed_data 可能包含多个 TLS record
    while (true) {
        int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            plaintext.append(buf, n);
            totalRead += n;
            continue;
        }

        int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ) {
            // 没有更多数据可读，正常退出
            break;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            // 对端发起 TLS 关闭通知
            spdlog::debug("TlsConnection: Peer sent TLS close_notify");
            break;
        }

        // 真正的错误
        state_ = State::ERROR;
        spdlog::error("TlsConnection: SSL_read error, SSL_get_error={}", err);
        return -1;
    }

    return totalRead;
}

int TlsConnection::encrypt(const char* data, size_t len) {
    if (state_ != State::ESTABLISHED) {
        spdlog::warn("TlsConnection: Cannot encrypt, TLS not established");
        return -1;
    }

    if (!data || len == 0) return 0;

    // SSL_write 将明文加密后写入 wbio
    int written = SSL_write(ssl_, data, static_cast<int>(len));
    if (written <= 0) {
        int err = SSL_get_error(ssl_, written);
        if (err == SSL_ERROR_WANT_WRITE) {
            // 需要先 flush wbio（实际上 Memory BIO 不应该出现这种情况）
            return 0;
        }
        state_ = State::ERROR;
        spdlog::error("TlsConnection: SSL_write error, SSL_get_error={}", err);
        return -1;
    }

    return written;
}

std::string TlsConnection::get_output() {
    std::string output;

    // 从写 BIO 读取所有待发送的密文数据
    int pending = BIO_ctrl_pending(wbio_);
    if (pending <= 0) {
        return output;
    }

    output.resize(pending);
    int n = BIO_read(wbio_, &output[0], pending);
    if (n <= 0) {
        output.clear();
        return output;
    }
    output.resize(n);

    return output;
}
