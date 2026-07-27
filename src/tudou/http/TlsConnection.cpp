// ============================================================================
// 单连接 TLS 会话实现：通过 Memory BIO 推进握手、解密输入并加密输出。
// 所有调用均由所属 EventLoop 串行执行，OpenSSL 不直接读写 Socket。
// ============================================================================

#include "tudou/http/TlsConnection.h"

#include "spdlog/spdlog.h"

namespace {

constexpr int kTlsBufferSize = 16384;

} // namespace

TlsConnection::TlsConnection(SSL* ssl)
    : ssl_(ssl) {

    if (!ssl_) {
        mark_error("TlsConnection: Cannot initialize with null SSL handle");
        return;
    }

    BIO* rbio = BIO_new(BIO_s_mem());
    BIO* wbio = BIO_new(BIO_s_mem());
    if (!rbio || !wbio) {
        if (rbio) {
            BIO_free(rbio);
        }
        if (wbio) {
            BIO_free(wbio);
        }
        mark_error("TlsConnection: Failed to create Memory BIO pair");
        return;
    }

    SSL_set_bio(ssl_, rbio, wbio);
    SSL_set_accept_state(ssl_);
}

TlsConnection::~TlsConnection() {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}

TlsConnection::ReadResult TlsConnection::read_plaintext(const std::string& ciphertext, std::string& plaintext, std::string& outboundCiphertext) {
    plaintext.clear();
    outboundCiphertext.clear();

    if (is_error()) {
        spdlog::error("TlsConnection: Cannot read_plaintext, TLS session is in error state");
        return ReadResult::Error;
    }

    // 1. 先把本次 socket 收到的密文交给 OpenSSL。
    if (!ciphertext.empty()) {
        const int written = BIO_write(SSL_get_rbio(ssl_), ciphertext.data(), static_cast<int>(ciphertext.size()));
        if (written <= 0) {
            mark_error("TlsConnection: BIO_write failed");
            return ReadResult::Error;
        }
    }

    // 2. 握手未完成时只推进握手，不把握手字节误当作 HTTP 数据。
    if (!is_established() && !advance_handshake()) {
        return ReadResult::Error;
    }

    // 3. 无论握手是否完成，先取出 OpenSSL 本轮产生的待发密文。
    outboundCiphertext = drain_ciphertext();
    if (!is_established()) {
        return ReadResult::NeedMoreData;
    }

    // 4. 握手完成后，读尽当前已经解出的 HTTP 明文。
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

    if (is_error()) {
        spdlog::error("TlsConnection: Cannot write_plaintext, TLS session is in error state");
        return false;
    }

    if (!is_established()) {
        spdlog::warn("TlsConnection: Cannot encrypt, TLS not established");
        return false;
    }

    const int written = SSL_write(ssl_, plaintext.data(), static_cast<int>(plaintext.size()));
    if (written <= 0) {
        const int err = SSL_get_error(ssl_, written);
        mark_error("TlsConnection: SSL_write failed");
        spdlog::error("TlsConnection: SSL_write error, SSL_get_error={}", err);
        return false;
    }

    ciphertext = drain_ciphertext();
    return true;
}

bool TlsConnection::advance_handshake() {
    const int result = SSL_do_handshake(ssl_);
    if (result == 1) {
        spdlog::debug("TlsConnection: TLS handshake completed successfully");
        return true;
    }

    const int err = SSL_get_error(ssl_, result);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return true;
    }

    mark_error("TlsConnection: TLS handshake failed");
    return false;
}

std::string TlsConnection::drain_ciphertext() {
    std::string output;

    BIO* wbio = SSL_get_wbio(ssl_);
    const int pending = BIO_ctrl_pending(wbio);
    if (pending <= 0) {
        return output;
    }

    output.resize(pending);
    int n = BIO_read(wbio, &output[0], pending);
    if (n <= 0) {
        output.clear();
        return output;
    }
    output.resize(n);

    return output;
}

void TlsConnection::mark_error(const char* message) {
    spdlog::error("{}", message);
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}
