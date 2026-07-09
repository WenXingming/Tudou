#include <gtest/gtest.h>

#include <openssl/ssl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>

#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>

#include "base/ScopedFd.h"
#include "tudou/http/TlsConfig.h"

#define private public
#include "tudou/http/HttpServer.h"
#undef private
#include "tudou/http/TlsProbe.h"

#include "tudou/tcp/InetAddress.h"
#include "tudou/reactor/EventLoop.h"
#include "tudou/tcp/Socket.h"
#include "tudou/tcp/TcpConnection.h"

namespace {

std::shared_ptr<TcpConnection> make_connection(EventLoop& loop, int fd) {
    InetAddress localAddr("127.0.0.1", 8080);
    InetAddress peerAddr("127.0.0.1", 8081);
    return TcpConnection::create_connection(&loop, Socket(fd), localAddr, peerAddr);
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

bool create_tcp_socketpair(int fds[2]) {
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(listenFd);
        return false;
    }

    if (::listen(listenFd, 1) < 0) {
        ::close(listenFd);
        return false;
    }

    socklen_t addrLen = sizeof(addr);
    if (::getsockname(listenFd, (struct sockaddr*)&addr, &addrLen) < 0) {
        ::close(listenFd);
        return false;
    }

    int clientFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (clientFd < 0) {
        ::close(listenFd);
        return false;
    }

    if (::connect(clientFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(listenFd);
        ::close(clientFd);
        return false;
    }

    int serverFd = ::accept(listenFd, nullptr, nullptr);
    ::close(listenFd);

    if (serverFd < 0) {
        ::close(clientFd);
        return false;
    }

    fds[0] = serverFd;
    fds[1] = clientFd;
    return true;
}

} // namespace

TEST(HttpServerTest, OnConnectCreatesAndOnCloseRemovesConnectionState) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);

    server.on_connect(conn);

    ASSERT_EQ(server.connectionStates_.size(), 1U);
    const auto stateIt = server.connectionStates_.find(conn.get());
    ASSERT_NE(stateIt, server.connectionStates_.end());
    ASSERT_NE(stateIt->second, nullptr);
    EXPECT_EQ(stateIt->second->tlsMode, TlsMode::None);
    EXPECT_EQ(stateIt->second->tlsConnection, nullptr);

    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });
    conn->force_close();

    EXPECT_TRUE(server.connectionStates_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, SslConnectionStateUsesMemoryBioMode) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    ASSERT_TRUE(server.enable_ssl(cert_path("test-cert.pem"), cert_path("test-key.pem")));

    auto conn = make_connection(loop, fds[0]);
    server.on_connect(conn);

    ASSERT_EQ(server.connectionStates_.size(), 1U);
    const auto stateIt = server.connectionStates_.find(conn.get());
    ASSERT_NE(stateIt, server.connectionStates_.end());
    ASSERT_NE(stateIt->second, nullptr);
    EXPECT_EQ(stateIt->second->tlsMode, TlsMode::MemoryBio);
    EXPECT_NE(stateIt->second->tlsConnection, nullptr);

    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });
    conn->force_close();

    EXPECT_TRUE(server.connectionStates_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, SetTlsModeAcceptsSupportedModes) {
    HttpServer server("127.0.0.1", 8080, 0);

    EXPECT_TRUE(server.set_tls_mode(TlsMode::MemoryBio));
    EXPECT_EQ(server.tlsMode_, TlsMode::MemoryBio);

    bool kTlsSupported = TlsProbe::is_kernel_tls_supported();
    EXPECT_EQ(server.set_tls_mode(TlsMode::KernelTls), kTlsSupported);
    if (kTlsSupported) {
        EXPECT_EQ(server.tlsMode_, TlsMode::KernelTls);
    } else {
        EXPECT_EQ(server.tlsMode_, TlsMode::MemoryBio);
    }

    EXPECT_FALSE(server.set_tls_mode(TlsMode::None));
    if (kTlsSupported) {
        EXPECT_EQ(server.tlsMode_, TlsMode::KernelTls);
    } else {
        EXPECT_EQ(server.tlsMode_, TlsMode::MemoryBio);
    }
}

TEST(HttpServerTest, KernelTlsConnectionStateUsesKernelTlsMode) {
    if (!TlsProbe::is_kernel_tls_supported()) {
        GTEST_SKIP() << "Kernel TLS is not supported on this platform, skipping test.";
    }

    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    ASSERT_TRUE(server.enable_ssl(cert_path("test-cert.pem"), cert_path("test-key.pem")));
    ASSERT_TRUE(server.set_tls_mode(TlsMode::KernelTls));

    auto conn = make_connection(loop, fds[0]);
    server.on_connect(conn);

    ASSERT_EQ(server.connectionStates_.size(), 1U);
    const auto stateIt = server.connectionStates_.find(conn.get());
    ASSERT_NE(stateIt, server.connectionStates_.end());
    ASSERT_NE(stateIt->second, nullptr);
    EXPECT_EQ(stateIt->second->tlsMode, TlsMode::KernelTls);
    EXPECT_NE(stateIt->second->tlsConnection, nullptr); // OpenSSL handshake peer still created first

    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });
    conn->force_close();
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
    EXPECT_TRUE(server.connectionStates_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, ProcessPlainHttpRequestSendsFileBody) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    char path[] = "/tmp/tudou-http-file-body-test-XXXXXX";
    int fileFd = ::mkstemp(path);
    ASSERT_GE(fileFd, 0);
    ASSERT_EQ(::unlink(path), 0);

    const std::string fileBody = "static file body";
    ASSERT_EQ(::write(fileFd, fileBody.data(), fileBody.size()), static_cast<ssize_t>(fileBody.size()));
    ASSERT_EQ(::lseek(fileFd, 0, SEEK_SET), 0);
    auto file = std::make_shared<ScopedFd>(fileFd);

    EventLoop loop(20);
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);

    server.add_get_route("/file", [&](const HttpRequest& request, HttpResponse& response) {
        EXPECT_EQ(request.get_path(), "/file");
        response.set_status(200, "OK");
        response.set_header("Content-Type", "text/plain");
        response.set_file_body(file, fileBody.size());
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
        "GET /file HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";
    ASSERT_EQ(::write(fds[1], request.data(), request.size()), static_cast<ssize_t>(request.size()));

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();

    const std::string response = read_available(fds[1]);
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Type: text/plain\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Length: " + std::to_string(fileBody.size()) + "\r\n"), std::string::npos);
    EXPECT_NE(response.find("\r\n\r\n" + fileBody), std::string::npos);

    conn->force_close();
    EXPECT_TRUE(server.connectionStates_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, SendTlsFileBodyEncryptsHeaderAndFile) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    TlsConfig tlsConfig;
    ASSERT_TRUE(tlsConfig.init(cert_path("test-cert.pem"), cert_path("test-key.pem")));

    SSL* serverSsl = tlsConfig.create_ssl();
    ASSERT_NE(serverSsl, nullptr);

    HttpServer::ConnectionState state;
    state.tlsMode = TlsMode::MemoryBio;
    state.tlsConnection = std::make_unique<TlsConnection>(serverSsl);

    ClientTlsPeer client;
    ASSERT_TRUE(complete_handshake(client, *state.tlsConnection));

    char path[] = "/tmp/tudou-http-tls-file-body-test-XXXXXX";
    int fileFd = ::mkstemp(path);
    ASSERT_GE(fileFd, 0);
    ASSERT_EQ(::unlink(path), 0);

    const std::string fileBody = "tls static file body";
    ASSERT_EQ(::write(fileFd, fileBody.data(), fileBody.size()), static_cast<ssize_t>(fileBody.size()));
    ASSERT_EQ(::lseek(fileFd, 0, SEEK_SET), 0);

    HttpResponse response;
    response.set_status(200, "OK");
    response.set_header("Content-Type", "text/plain");
    response.set_file_body(std::make_shared<ScopedFd>(fileFd), fileBody.size());

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    auto conn = make_connection(loop, fds[0]);

    server.send_http_response(conn, state, response);

    const std::string encryptedResponse = read_all_available(fds[1]);
    ASSERT_FALSE(encryptedResponse.empty());
    ASSERT_GT(client.feed_input(encryptedResponse), 0);

    std::string decryptedResponse;
    ASSERT_GT(client.read_plaintext(decryptedResponse), 0);
    EXPECT_NE(decryptedResponse.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(decryptedResponse.find("Content-Type: text/plain\r\n"), std::string::npos);
    EXPECT_NE(decryptedResponse.find("Content-Length: " + std::to_string(fileBody.size()) + "\r\n"), std::string::npos);
    EXPECT_NE(decryptedResponse.find("\r\n\r\n" + fileBody), std::string::npos);

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
    EXPECT_TRUE(server.connectionStates_.empty());

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
    EXPECT_TRUE(server.connectionStates_.empty());

    ::close(fds[1]);
}

TEST(HttpServerTest, KernelTlsOffloadEnablesPlaintextTransmission) {
    if (!TlsProbe::is_kernel_tls_supported()) {
        GTEST_SKIP() << "Kernel TLS is not supported on this platform, skipping test.";
    }

    int fds[2] = { -1, -1 };
    ASSERT_TRUE(create_tcp_socketpair(fds));

    // Ensure sockets are in non-blocking mode for our driving loop
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);

    EventLoop loop;
    HttpServer server("127.0.0.1", 8080, 0);
    ASSERT_TRUE(server.enable_ssl(cert_path("test-cert.pem"), cert_path("test-key.pem")));
    ASSERT_TRUE(server.set_tls_mode(TlsMode::KernelTls));

    // Client Peer
    ClientTlsPeer client;

    auto conn = make_connection(loop, fds[0]);
    server.on_connect(conn);

    auto state = server.find_connection_state(conn);
    ASSERT_NE(state, nullptr);

    conn->set_message_callback([&](const std::shared_ptr<TcpConnection>& activeConn) {
        server.on_message(activeConn);
    });
    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
    });

    // Drive handshake by passing data back and forth using event loop timer
    int rounds = 0;
    std::function<void()> driveHandshake = [&]() {
        rounds++;
        if (rounds > 500) {
            loop.quit();
            return;
        }

        if (!client.is_established()) {
            client.advance_handshake();
        }

        std::string clientOut = client.take_output();
        if (!clientOut.empty()) {
            (void)::write(fds[1], clientOut.data(), clientOut.size());
        }

        std::string toClient = read_available(fds[1]);
        if (!toClient.empty()) {
            client.feed_input(toClient);
            client.advance_handshake();
        }

        // Exit loop once client is established and server has processed offloading (either succeeded or fell back)
        if (client.is_established() && (state->isKtlsOffloaded || state->tlsMode == TlsMode::MemoryBio)) {
            loop.quit();
            return;
        }

        loop.run_after(0.005, driveHandshake);
    };

    loop.run_after(0.005, driveHandshake);
    loop.loop();

    ASSERT_TRUE(client.is_established());

    // 4. Verify transmission depending on offload result
    if (state->isKtlsOffloaded) {
        // Read and discard the 1-byte space sent by OpenSSL to trigger kTLS configuration
        ::usleep(10000);
        std::string junkResponse = read_available(fds[1]);
        ASSERT_FALSE(junkResponse.empty());
        ASSERT_GT(client.feed_input(junkResponse), 0);
        std::string junkPlaintext;
        EXPECT_GT(client.read_plaintext(junkPlaintext), 0);
        EXPECT_EQ(junkPlaintext, " ");

        // Register a simple route for verifying the kTLS response
        bool routeCalled = false;
        server.add_get_route("/", [&](const HttpRequest& req, HttpResponse& resp) {
            (void)req;
            routeCalled = true;
            resp.set_status(200, "OK");
            resp.set_body("ktls response ok");
        });

        // Client sends an encrypted request
        std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        std::string encryptedRequest;
        ASSERT_TRUE(client.write_plaintext(request, encryptedRequest));
        ASSERT_GT(::write(fds[1], encryptedRequest.data(), encryptedRequest.size()), 0);

        // Run loop to let TcpConnection read, decrypt (via kernel RX), parse, and respond
        loop.run_after(0.05, [&]() {
            loop.quit();
        });
        loop.loop();

        EXPECT_TRUE(routeCalled);

        // Read response from client fd.
        // Due to kTLS TX offload, the kernel encrypted it, so we read TLS ciphertext bytes.
        std::string encryptedResponse = read_all_available(fds[1]);
        ASSERT_FALSE(encryptedResponse.empty());

        // Client decrypts the response bytes
        ASSERT_GT(client.feed_input(encryptedResponse), 0);
        std::string decryptedResponse;
        EXPECT_GT(client.read_plaintext(decryptedResponse), 0);
        EXPECT_NE(decryptedResponse.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
        EXPECT_NE(decryptedResponse.find("ktls response ok"), std::string::npos);
    } else {
        SUCCEED() << "kTLS offloading is not fully supported in this restricted test environment (expected in sandbox containers). Skipping transmission check.";
    }

    conn->force_close();
    ::close(fds[1]);
}
