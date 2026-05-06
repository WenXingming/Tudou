// ============================================== //
// TlsConnection.cpp
// 单连接 TLS 状态机实现，统一处理握手、加解密与错误态收敛。
// ============================================== //

#include "TlsConnection.h"
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

    initialize_tls_session();
}

TlsConnection::~TlsConnection() {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}

int TlsConnection::feed_data(const char* data, size_t len) {
    if (!ensure_tls_session("feed_data")) {
        return -1;
    }

    if (!data || len == 0) {
        return 0;
    }

    // 所有网络密文都统一先写入 rbio，后续握手和解密步骤只消费内存中的标准输入。
    const int written = BIO_write(rbio_, data, static_cast<int>(len));
    if (written <= 0) {
        mark_error("TlsConnection: BIO_write failed");
        return -1;
    }

    return written;
}

bool TlsConnection::do_handshake() {
    if (!ensure_tls_session("do_handshake")) {
        return false;
    }

    const int result = SSL_do_handshake(ssl_);
    if (result == 1) {
        state_ = State::ESTABLISHED;
        spdlog::debug("TlsConnection: TLS handshake completed successfully");
        return true;
    }

    return handle_tls_progress("TlsConnection: TLS handshake failed", result);
}

int TlsConnection::decrypt(std::string& plaintext) {
    if (!ensure_tls_session("decrypt")) {
        return -1;
    }

    if (state_ != State::ESTABLISHED) {
        spdlog::warn("TlsConnection: Cannot decrypt, TLS not established");
        return -1;
    }

    char buf[kTlsBufferSize];
    int totalRead = 0;

    // 单次 feed_data 可能携带多个 TLS record，因此这里要持续抽干 SSL_read 直到 WANT_READ。
    while (true) {
        const int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            plaintext.append(buf, n);
            totalRead += n;
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
        return -1;
    }

    return totalRead;
}

int TlsConnection::encrypt(const char* data, size_t len) {
    if (!ensure_tls_session("encrypt")) {
        return -1;
    }

    if (state_ != State::ESTABLISHED) {
        spdlog::warn("TlsConnection: Cannot encrypt, TLS not established");
        return -1;
    }

    if (!data || len == 0) {
        return 0;
    }

    // 明文写入 SSL 后，加密结果统一滞留在 wbio，等待 HttpServer 拉取后发送。
    const int written = SSL_write(ssl_, data, static_cast<int>(len));
    if (written <= 0) {
        const int err = SSL_get_error(ssl_, written);
        if (err == SSL_ERROR_WANT_WRITE) {
            return 0;
        }
        mark_error("TlsConnection: SSL_write failed");
        spdlog::error("TlsConnection: SSL_write error, SSL_get_error={}", err);
        return -1;
    }

    return written;
}

std::string TlsConnection::get_output() {
    std::string output;

    if (!ssl_ || !wbio_) {
        return output;
    }

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

void TlsConnection::initialize_tls_session() {
    if (!ssl_) {
        mark_error("TlsConnection: Cannot initialize with null SSL handle");
        return;
    }

    if (!create_memory_bio_pair()) {
        mark_error("TlsConnection: Failed to create Memory BIO pair");
        return;
    }

    attach_memory_bio_pair();

    // 当前对象始终扮演 TLS 服务端，客户端握手驱动由对端承担。
    SSL_set_accept_state(ssl_);
}

bool TlsConnection::create_memory_bio_pair() {
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    if (rbio_ && wbio_) {
        return true;
    }

    if (rbio_) {
        BIO_free(rbio_);
        rbio_ = nullptr;
    }
    if (wbio_) {
        BIO_free(wbio_);
        wbio_ = nullptr;
    }

    return false;
}

void TlsConnection::attach_memory_bio_pair() {
    // SSL_set_bio 会接管 BIO 的释放职责；这里保留裸指针仅用于后续读写。
    SSL_set_bio(ssl_, rbio_, wbio_);
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

bool TlsConnection::handle_tls_progress(const char* action, int result) {
    const int err = SSL_get_error(ssl_, result);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return true;
    }

    state_ = State::ERROR;
    spdlog::error("{}, SSL_get_error={}", action, err);
    return false;
}
