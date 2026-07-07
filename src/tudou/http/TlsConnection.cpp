// ============================================================================
// TlsConnection.cpp
// 单连接 TLS 状态机实现，统一处理握手、加解密与错误态收敛。
// ============================================================================

#include "tudou/http/TlsConnection.h"
#include "spdlog/spdlog.h"

#include <openssl/ssl.h>

namespace {

constexpr int kTlsBufferSize = 16384;

} // namespace

TlsConnection::TlsConnection(SSL* ssl)
    : ssl_(ssl)
    , rbio_(nullptr)
    , wbio_(nullptr)
    , state_(State::HANDSHAKING) {

    if (!ssl_) {
        mark_error("TlsConnection: Cannot initialize with null SSL handle");
        return;
    }

    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    if (!rbio_ || !wbio_) {
        if (rbio_) {
            BIO_free(rbio_);
            rbio_ = nullptr;
        }
        if (wbio_) {
            BIO_free(wbio_);
            wbio_ = nullptr;
        }
        mark_error("TlsConnection: Failed to create Memory BIO pair");
        return;
    }

    // SSL_set_bio 会接管 BIO 的释放职责；这里保留裸指针仅用于后续读写。
    SSL_set_bio(ssl_, rbio_, wbio_);
    // 当前对象始终扮演 TLS 服务端，客户端握手驱动由对端承担。
    SSL_set_accept_state(ssl_);
}

TlsConnection::~TlsConnection() {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}

TlsConnection::ReadResult TlsConnection::read_plaintext(
    const std::string& ciphertext,
    std::string& plaintext,
    std::string& outboundCiphertext) {
    plaintext.clear();
    outboundCiphertext.clear();

    if (!ensure_tls_session("read_plaintext")) {
        return ReadResult::Error;
    }

    // 1. 将接收到的网络密文写入输入缓冲（rbio_）
    if (!ciphertext.empty()) {
        const int written = BIO_write(rbio_, ciphertext.data(), static_cast<int>(ciphertext.size()));
        if (written <= 0) {
            mark_error("TlsConnection: BIO_write failed");
            return ReadResult::Error;
        }
    }

    // 2. 尝试推进 TLS 握手状态
    if (state_ == State::HANDSHAKING) {
        const int result = SSL_do_handshake(ssl_);
        if (result == 1) {
            state_ = State::ESTABLISHED;
            spdlog::debug("TlsConnection: TLS handshake completed successfully");
        } else {
            const int err = SSL_get_error(ssl_, result);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                state_ = State::ERROR;
                spdlog::error("TlsConnection: TLS handshake failed, SSL_get_error={}", err);
                return ReadResult::Error;
            }
        }
    }

    // 先把握手阶段产生的待发密文交给调用方，再决定本轮是否已有可读明文。
    outboundCiphertext = drain_ciphertext();
    if (state_ == State::HANDSHAKING) {
        return ReadResult::NeedMoreData;
    }

    if (!is_established()) {
        spdlog::error("TlsConnection: TLS session left handshake without entering ESTABLISHED");
        return ReadResult::Error;
    }

    // 3. 从 SSL 会话中持续读取并解密应用层明文
    char buf[kTlsBufferSize];
    while (true) {
        const int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            plaintext.append(buf, n);
            continue;
        }

        const int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ) {
            break;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            spdlog::debug("TlsConnection: Peer sent TLS close_notify");
            break;
        }

        mark_error("TlsConnection: SSL_read failed");
        spdlog::error("TlsConnection: SSL_read error, SSL_get_error={}", err);
        return ReadResult::Error;
    }

    return plaintext.empty() ? ReadResult::NeedMoreData : ReadResult::Ready;
}

bool TlsConnection::write_plaintext(const std::string& plaintext, std::string& ciphertext) {
    ciphertext.clear();
    if (plaintext.empty()) {
        return true;
    }

    if (!ensure_tls_session("write_plaintext")) {
        return false;
    }

    if (state_ != State::ESTABLISHED) {
        spdlog::warn("TlsConnection: Cannot encrypt, TLS not established");
        return false;
    }

    // 明文写入 SSL 后，加密结果统一滞留在 wbio，等待 HttpServer 拉取后发送。
    const int written = SSL_write(ssl_, plaintext.data(), static_cast<int>(plaintext.size()));
    if (written <= 0) {
        const int err = SSL_get_error(ssl_, written);
        if (err != SSL_ERROR_WANT_WRITE) {
            mark_error("TlsConnection: SSL_write failed");
            spdlog::error("TlsConnection: SSL_write error, SSL_get_error={}", err);
            return false;
        }
        return true;
    }

    ciphertext = drain_ciphertext();
    return !ciphertext.empty();
}

std::string TlsConnection::drain_ciphertext() {
    std::string output;

    if (!ssl_ || !wbio_) {
        return output;
    }

    // OpenSSL 把待发送密文积压在写 BIO 里，这里统一抽干交给调用方发送。
    const int pending = BIO_ctrl_pending(wbio_);
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

bool TlsConnection::ensure_tls_session(const char* action) const {
    if (!ssl_ || !rbio_ || !wbio_) {
        spdlog::error("TlsConnection: Cannot {}, TLS session not initialized", action);
        return false;
    }

    if (state_ == State::ERROR) {
        spdlog::error("TlsConnection: Cannot {}, TLS session already in error state", action);
        return false;
    }

    return true;
}

void TlsConnection::mark_error(const char* message) {
    state_ = State::ERROR;
    spdlog::error("{}", message);
}
