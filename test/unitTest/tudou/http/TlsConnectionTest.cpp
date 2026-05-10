#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <string>

#include "tudou/http/SslContext.h"
#include "tudou/http/TlsConnection.h"

namespace {

std::string cert_path(const char* fileName) {
    return std::string(TUDOU_SOURCE_DIR) + "/certs/" + fileName;
}

class ClientTlsPeer {
public:
    ClientTlsPeer()
        : context_(SSL_CTX_new(TLS_client_method()))
        , ssl_(nullptr)
        , rbio_(nullptr)
        , wbio_(nullptr) {
        EXPECT_NE(context_, nullptr);

        SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION);
        SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);

        ssl_ = SSL_new(context_);
        EXPECT_NE(ssl_, nullptr);

        rbio_ = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());
        EXPECT_NE(rbio_, nullptr);
        EXPECT_NE(wbio_, nullptr);

        SSL_set_bio(ssl_, rbio_, wbio_);
        SSL_set_connect_state(ssl_);
    }

    ~ClientTlsPeer() {
        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (context_) {
            SSL_CTX_free(context_);
            context_ = nullptr;
        }
    }

    bool advance_handshake() {
        const int result = SSL_do_handshake(ssl_);
        if (result == 1) {
            return true;
        }

        const int err = SSL_get_error(ssl_, result);
        return err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
    }

    bool is_established() const {
        return SSL_is_init_finished(ssl_) == 1;
    }

    int feed_input(const std::string& encrypted) {
        if (encrypted.empty()) {
            return 0;
        }

        return BIO_write(rbio_, encrypted.data(), static_cast<int>(encrypted.size()));
    }

    std::string take_output() {
        std::string output;
        const int pending = BIO_ctrl_pending(wbio_);
        if (pending <= 0) {
            return output;
        }

        output.resize(pending);
        const int readSize = BIO_read(wbio_, &output[0], pending);
        if (readSize <= 0) {
            output.clear();
            return output;
        }

        output.resize(readSize);
        return output;
    }

    bool write_plaintext(const std::string& plaintext) {
        return SSL_write(ssl_, plaintext.data(), static_cast<int>(plaintext.size())) > 0;
    }

    int read_plaintext(std::string& plaintext) {
        char buffer[4096];
        int totalRead = 0;

        while (true) {
            const int readSize = SSL_read(ssl_, buffer, sizeof(buffer));
            if (readSize > 0) {
                plaintext.append(buffer, readSize);
                totalRead += readSize;
                continue;
            }

            const int err = SSL_get_error(ssl_, readSize);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_ZERO_RETURN) {
                break;
            }

            return -1;
        }

        return totalRead;
    }

private:
    SSL_CTX* context_;
    SSL* ssl_;
    BIO* rbio_;
    BIO* wbio_;
};

bool complete_handshake(ClientTlsPeer& client, TlsConnection& server) {
    for (int round = 0; round < 32; ++round) {
        if (!client.is_established() && !client.advance_handshake()) {
            return false;
        }

        std::string serverPlaintext;
        std::string serverOutput;
        const TlsConnection::ReadResult readResult =
            server.read_plaintext(client.take_output(), serverPlaintext, serverOutput);
        if (readResult == TlsConnection::ReadResult::Error) {
            return false;
        }
        if (!serverPlaintext.empty()) {
            return false;
        }
        if (!serverOutput.empty() && client.feed_input(serverOutput) < 0) {
            return false;
        }

        if (client.is_established() && server.is_established()) {
            return true;
        }
    }

    return false;
}

} // namespace

TEST(TlsConnectionTest, NullSslHandleTransitionsToErrorState) {
    TlsConnection connection(nullptr);
    std::string plaintext;
    std::string ciphertext;

    EXPECT_TRUE(connection.is_error());
    EXPECT_EQ(connection.read_plaintext("x", plaintext, ciphertext), TlsConnection::ReadResult::Error);
    EXPECT_FALSE(connection.write_plaintext("x", ciphertext));
    EXPECT_TRUE(ciphertext.empty());
}

TEST(TlsConnectionTest, HandshakeEncryptAndDecryptRoundTrip) {
    SslContext serverContext;
    ASSERT_TRUE(serverContext.init(cert_path("test-cert.pem"), cert_path("test-key.pem")));

    SSL* serverSsl = serverContext.create_ssl();
    ASSERT_NE(serverSsl, nullptr);

    TlsConnection serverConnection(serverSsl);
    ClientTlsPeer clientPeer;

    ASSERT_TRUE(complete_handshake(clientPeer, serverConnection));
    ASSERT_TRUE(serverConnection.is_established());

    const std::string request = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_TRUE(clientPeer.write_plaintext(request));

    std::string decryptedRequest;
    std::string serverOutput;
    ASSERT_EQ(serverConnection.read_plaintext(clientPeer.take_output(), decryptedRequest, serverOutput),
        TlsConnection::ReadResult::Ready);
    EXPECT_TRUE(serverOutput.empty());
    EXPECT_EQ(decryptedRequest, request);

    const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    std::string encryptedResponse;
    ASSERT_TRUE(serverConnection.write_plaintext(response, encryptedResponse));
    ASSERT_FALSE(encryptedResponse.empty());
    ASSERT_GT(clientPeer.feed_input(encryptedResponse), 0);

    std::string decryptedResponse;
    EXPECT_EQ(clientPeer.read_plaintext(decryptedResponse), static_cast<int>(response.size()));
    EXPECT_EQ(decryptedResponse, response);
}