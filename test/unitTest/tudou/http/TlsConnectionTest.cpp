#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <string>

#include "tudou/http/SslContext.h"
#include "tudou/http/TlsConnection.h"

namespace {

constexpr char kValidCertPath[] = "/workspaces/Tudou/certs/test-cert.pem";
constexpr char kValidKeyPath[] = "/workspaces/Tudou/certs/test-key.pem";

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

        const std::string clientOutput = client.take_output();
        if (!clientOutput.empty() && server.feed_data(clientOutput.data(), clientOutput.size()) < 0) {
            return false;
        }

        if (!server.is_established() && !server.do_handshake()) {
            return false;
        }

        const std::string serverOutput = server.get_output();
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

    EXPECT_TRUE(connection.is_error());
    EXPECT_FALSE(connection.do_handshake());
    EXPECT_EQ(connection.feed_data("x", 1), -1);
    EXPECT_EQ(connection.encrypt("x", 1), -1);
    EXPECT_TRUE(connection.get_output().empty());
}

TEST(TlsConnectionTest, HandshakeEncryptAndDecryptRoundTrip) {
    SslContext serverContext;
    ASSERT_TRUE(serverContext.init(kValidCertPath, kValidKeyPath));

    SSL* serverSsl = serverContext.create_ssl();
    ASSERT_NE(serverSsl, nullptr);

    TlsConnection serverConnection(serverSsl);
    ClientTlsPeer clientPeer;

    ASSERT_TRUE(complete_handshake(clientPeer, serverConnection));
    ASSERT_TRUE(serverConnection.is_established());

    const std::string request = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_TRUE(clientPeer.write_plaintext(request));

    const std::string encryptedRequest = clientPeer.take_output();
    ASSERT_FALSE(encryptedRequest.empty());
    ASSERT_GT(serverConnection.feed_data(encryptedRequest.data(), encryptedRequest.size()), 0);

    std::string decryptedRequest;
    EXPECT_EQ(serverConnection.decrypt(decryptedRequest), static_cast<int>(request.size()));
    EXPECT_EQ(decryptedRequest, request);

    const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    ASSERT_EQ(serverConnection.encrypt(response.data(), response.size()), static_cast<int>(response.size()));

    const std::string encryptedResponse = serverConnection.get_output();
    ASSERT_FALSE(encryptedResponse.empty());
    ASSERT_GT(clientPeer.feed_input(encryptedResponse), 0);

    std::string decryptedResponse;
    EXPECT_EQ(clientPeer.read_plaintext(decryptedResponse), static_cast<int>(response.size()));
    EXPECT_EQ(decryptedResponse, response);
}