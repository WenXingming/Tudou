#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>

#include "tudou/http/TlsConfig.h"

#define private public
#include "tudou/http/HttpServer.h"
#undef private

#include "tudou/tcp/InetAddress.h"
#include "tudou/reactor/EventLoop.h"
#include "tudou/tcp/Socket.h"
#include "tudou/tcp/TcpConnection.h"

namespace {

std::shared_ptr<TcpConnection> make_connection(EventLoop& loop, int fd) {
    InetAddress peerAddr("127.0.0.1", 8081);
    return TcpConnection::create_connection(&loop, Socket(fd), peerAddr);
}

std::string read_available(int fd) {
    char buffer[1024];
    const ssize_t nread = ::read(fd, buffer, sizeof(buffer));
    if (nread <= 0) {
        return "";
    }

    return std::string(buffer, static_cast<size_t>(nread));
}

std::string read_all_available(int fd) {
    std::string output;
    char buffer[4096];
    while (true) {
        const ssize_t nread = ::read(fd, buffer, sizeof(buffer));
        if (nread <= 0) {
            break;
        }
        output.append(buffer, static_cast<size_t>(nread));
    }
    return output;
}

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
        }
        if (context_) {
            SSL_CTX_free(context_);
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
        const int nread = BIO_read(wbio_, &output[0], pending);
        if (nread <= 0) {
            output.clear();
            return output;
        }
        output.resize(nread);
        return output;
    }

    int read_plaintext(std::string& plaintext) {
        char buffer[4096];
        int total = 0;
        while (true) {
            const int nread = SSL_read(ssl_, buffer, sizeof(buffer));
            if (nread > 0) {
                plaintext.append(buffer, nread);
                total += nread;
                continue;
            }

            const int err = SSL_get_error(ssl_, nread);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_ZERO_RETURN) {
                break;
            }
            return -1;
        }
        return total;
    }

    int write_plaintext(const std::string& plaintext, std::string& ciphertext) {
        ciphertext.clear();
        int n = SSL_write(ssl_, plaintext.data(), static_cast<int>(plaintext.size()));
        if (n > 0) {
            ciphertext = take_output();
        }
        return n;
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
        const TlsConnection::ReadResult result =
            server.read_plaintext(client.take_output(), serverPlaintext, serverOutput);
        if (result == TlsConnection::ReadResult::Error || !serverPlaintext.empty()) {
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

TEST(HttpServerTest, OnConnectCreatesAndOnCloseRemovesHttpConnection) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);

    server.on_connect(conn);

    ASSERT_EQ(server.httpConnections_.size(), 1U);
    const auto httpConnectionIt = server.httpConnections_.find(conn.get());
    ASSERT_NE(httpConnectionIt, server.httpConnections_.end());
    ASSERT_NE(httpConnectionIt->second, nullptr);

    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });
    conn->force_close();

    EXPECT_TRUE(server.httpConnections_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, SslHttpConnectionCreatesTlsConnection) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    ASSERT_TRUE(server.enable_ssl(cert_path("test-cert.pem"), cert_path("test-key.pem")));

    auto conn = make_connection(loop, fds[0]);
    server.on_connect(conn);

    ASSERT_EQ(server.httpConnections_.size(), 1U);
    const auto httpConnectionIt = server.httpConnections_.find(conn.get());
    ASSERT_NE(httpConnectionIt, server.httpConnections_.end());
    ASSERT_NE(httpConnectionIt->second, nullptr);
    ClientTlsPeer client;
    ASSERT_TRUE(client.advance_handshake());

    std::vector<HttpRequest> requests;
    std::string outboundCiphertext;
    EXPECT_EQ(httpConnectionIt->second->decode_requests(client.take_output(), requests, outboundCiphertext),
        HttpConnection::ProcessResult::Success);
    EXPECT_TRUE(requests.empty());
    EXPECT_FALSE(outboundCiphertext.empty());

    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });
    conn->force_close();

    EXPECT_TRUE(server.httpConnections_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, ProcessPlainHttpRequestDispatchesRegisteredRouteAndSendsResponse) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop(20);
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);
    bool routeCalled = false;

    server.add_get_route("/hello", [&](const HttpRequest& request, HttpResponse& response) {
        routeCalled = true;
        EXPECT_EQ(request.get_path(), "/hello");
        response.set_status(201, "Created");
        response.set_body("ok");
        response.set_header("Content-Type", "text/plain");
        });
    server.on_connect(conn);

    conn->set_message_callback([&](const std::shared_ptr<TcpConnection>& activeConn) {
        server.on_message(activeConn);
        });
    conn->set_write_complete_callback([&](const std::shared_ptr<TcpConnection>&) {
        loop.quit();
        });
    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });

    const std::string request =
        "GET /hello HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";
    ASSERT_EQ(::write(fds[1], request.data(), request.size()), static_cast<ssize_t>(request.size()));

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();

    const std::string response = read_available(fds[1]);
    EXPECT_TRUE(routeCalled);
    EXPECT_NE(response.find("HTTP/1.1 201 Created\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Type: text/plain\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Length: 2\r\n"), std::string::npos);
    EXPECT_NE(response.find("\r\n\r\nok"), std::string::npos);

    conn->force_close();
    EXPECT_TRUE(server.httpConnections_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, SendTlsResponseEncryptsHeaderAndBody) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    TlsConfig tlsConfig;
    ASSERT_TRUE(tlsConfig.init(cert_path("test-cert.pem"), cert_path("test-key.pem")));

    SSL* serverSsl = tlsConfig.create_ssl_session();
    ASSERT_NE(serverSsl, nullptr);

    auto tlsConnection = std::make_unique<TlsConnection>(serverSsl);

    ClientTlsPeer client;
    ASSERT_TRUE(complete_handshake(client, *tlsConnection));

    HttpConnection httpConnection(std::move(tlsConnection));

    const std::string body = "tls response body";

    HttpResponse response;
    response.set_status(200, "OK");
    response.set_header("Content-Type", "text/plain");
    response.set_body(body);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);

    EXPECT_FALSE(server.send_http_response(conn, httpConnection, response));

    const std::string encryptedResponse = read_all_available(fds[1]);
    ASSERT_FALSE(encryptedResponse.empty());
    ASSERT_GT(client.feed_input(encryptedResponse), 0);

    std::string decryptedResponse;
    ASSERT_GT(client.read_plaintext(decryptedResponse), 0);
    EXPECT_NE(decryptedResponse.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(decryptedResponse.find("Content-Type: text/plain\r\n"), std::string::npos);
    EXPECT_NE(decryptedResponse.find("Content-Length: " + std::to_string(body.size()) + "\r\n"), std::string::npos);
    EXPECT_NE(decryptedResponse.find("\r\n\r\n" + body), std::string::npos);

    ::close(fds[1]);
}

TEST(HttpServerTest, SendHttpResponseClosesConnectionWhenRequested) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);
    HttpConnection httpConnection;

    HttpResponse response;
    response.set_header("Connection", "close");

    EXPECT_TRUE(server.send_http_response(conn, httpConnection, response));

    ::close(fds[1]);
}

TEST(HttpServerTest, SendHttpResponseClosesConnectionWhenTlsEncodingFails) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);
    bool closeCalled = false;
    conn->set_close_callback([&](const TcpConnectionPtr&) {
        closeCalled = true;
        });

    HttpConnection httpConnection(std::make_unique<TlsConnection>(nullptr));
    HttpResponse response;

    EXPECT_TRUE(server.send_http_response(conn, httpConnection, response));
    EXPECT_TRUE(closeCalled);

    ::close(fds[1]);
}

TEST(HttpServerTest, TlsReadFailureClosesConnection) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop(20);
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);
    server.on_connect(conn);
    server.httpConnections_[conn.get()] =
        std::make_shared<HttpConnection>(std::make_unique<TlsConnection>(nullptr));

    bool closeCalled = false;
    conn->set_message_callback([&](const TcpConnectionPtr& activeConn) {
        server.on_message(activeConn);
        });
    conn->set_close_callback([&](const TcpConnectionPtr&) {
        closeCalled = true;
        server.on_close(conn);
        loop.quit();
        });

    const std::string invalidTls = "invalid TLS";
    ASSERT_EQ(::write(fds[1], invalidTls.data(), invalidTls.size()), static_cast<ssize_t>(invalidTls.size()));
    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();

    EXPECT_TRUE(closeCalled);
    EXPECT_TRUE(server.httpConnections_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, ProcessBadRequestSendsBadRequestAndResetsContext) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop(20);
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);

    server.on_connect(conn);
    conn->set_message_callback([&](const std::shared_ptr<TcpConnection>& activeConn) {
        server.on_message(activeConn);
        });
    conn->set_write_complete_callback([&](const std::shared_ptr<TcpConnection>&) {
        loop.quit();
        });
    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });

    const std::string request =
        "GET /broken HTTP/1.1\r\n"
        "Host example.com\r\n"
        "\r\n";
    ASSERT_EQ(::write(fds[1], request.data(), request.size()), static_cast<ssize_t>(request.size()));

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();

    const std::string response = read_available(fds[1]);
    EXPECT_NE(response.find("HTTP/1.1 400 Bad Request\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Type: text/plain\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Length: 11\r\n"), std::string::npos);
    EXPECT_NE(response.find("\r\n\r\nBad Request"), std::string::npos);

    // 因 Connection: close 触发主动关闭，连接状态已在 on_close 回调中被物理清理
    EXPECT_TRUE(server.httpConnections_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, ProcessPipelinedRequests) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop(20);
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);
    int routeCalls = 0;

    server.add_get_route("/first", [&](const HttpRequest& request, HttpResponse& response) {
        routeCalls++;
        (void)request;
        response.set_status(200, "OK");
        response.set_body("first_response");
        });
    server.add_get_route("/second", [&](const HttpRequest& request, HttpResponse& response) {
        routeCalls++;
        (void)request;
        response.set_status(200, "OK");
        response.set_body("second_response");
        });

    server.on_connect(conn);
    conn->set_message_callback([&](const std::shared_ptr<TcpConnection>& activeConn) {
        server.on_message(activeConn);
        });
    conn->set_write_complete_callback([&](const std::shared_ptr<TcpConnection>&) {
        if (routeCalls == 2) {
            loop.quit();
        }
        });
    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });

    // 发送粘连的两个 HTTP 请求
    const std::string request =
        "GET /first HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n"
        "GET /second HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";
    ASSERT_EQ(::write(fds[1], request.data(), request.size()), static_cast<ssize_t>(request.size()));

    loop.run_after(0.5, [&]() {
        loop.quit();
        });
    loop.loop();

    const std::string response = read_available(fds[1]);
    EXPECT_EQ(routeCalls, 2);
    EXPECT_NE(response.find("first_response"), std::string::npos);
    EXPECT_NE(response.find("second_response"), std::string::npos);

    conn->force_close();
    EXPECT_TRUE(server.httpConnections_.empty());

    ::close(fds[1]);
}
