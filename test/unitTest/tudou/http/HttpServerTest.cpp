#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>

#define private public
#include "tudou/http/HttpServer.h"
#undef private

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
    EXPECT_EQ(stateIt->second->tlsConnection, nullptr);

    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        server.on_close(conn);
        });
    conn->force_close();

    EXPECT_TRUE(server.connectionStates_.empty());

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
