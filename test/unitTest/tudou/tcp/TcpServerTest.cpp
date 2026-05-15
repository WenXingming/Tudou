#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>
#include <thread>

#include "tudou/tcp/TcpServer.h"

namespace {

uint16_t reserve_free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1
        || ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }

    if (::close(fd) != 0) {
        return 0;
    }

    return ntohs(addr.sin_port);
}

int connect_with_retry(uint16_t port) {
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) != 1) {
        return -1;
    }

    for (int retry = 0; retry < 200; ++retry) {
        const int clientFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (clientFd < 0) {
            return -1;
        }

        if (::connect(clientFd, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) == 0) {
            return clientFd;
        }

        ::close(clientFd);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return -1;
}

std::thread start_watchdog(TcpServer& server, std::atomic<bool>& serverDone) {
    return std::thread([&]() {
        for (int retry = 0; retry < 100 && !serverDone.load(); ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!serverDone.load()) {
            server.stop();
        }
        });
}

std::string read_with_retry(int fd) {
    char buffer[1024];
    for (int retry = 0; retry < 200; ++retry) {
        const ssize_t nread = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (nread > 0) {
            return std::string(buffer, static_cast<size_t>(nread));
        }
        if (nread == 0) {
            return "";
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return "";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return "";
}

} // namespace

TEST(TcpServerTest, ConnectionLifecycleCallbacksFireInSingleThreadMode) {
    const uint16_t port = reserve_free_port();
    ASSERT_NE(port, 0);

    TcpServer server("127.0.0.1", port, 0);
    std::atomic<bool> connected{ false };
    std::atomic<bool> closed{ false };

    server.set_connection_callback([&](const TcpConnectionPtr& conn) {
        EXPECT_NE(conn, nullptr);
        connected = true;
        });
    server.set_close_callback([&](const TcpConnectionPtr& conn) {
        EXPECT_NE(conn, nullptr);
        closed = true;
        server.stop();
        });

    std::atomic<bool> serverDone{ false };
    std::thread serverThread([&]() {
        server.start();
        serverDone = true;
        });
    std::thread watchdog = start_watchdog(server, serverDone);

    const int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_EQ(::close(clientFd), 0);

    serverThread.join();
    watchdog.join();

    EXPECT_TRUE(connected.load());
    EXPECT_TRUE(closed.load());
}

TEST(TcpServerTest, StopExitsStartFromConnectionCallback) {
    const uint16_t port = reserve_free_port();
    ASSERT_NE(port, 0);

    TcpServer server("127.0.0.1", port, 0);
    std::atomic<bool> connected{ false };
    std::atomic<bool> serverDone{ false };

    server.set_connection_callback([&](const TcpConnectionPtr& conn) {
        EXPECT_NE(conn, nullptr);
        connected = true;
        server.stop();
        });

    std::thread serverThread([&]() {
        server.start();
        serverDone = true;
        });
    std::thread watchdog = start_watchdog(server, serverDone);

    const int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_EQ(::close(clientFd), 0);

    serverThread.join();
    watchdog.join();

    EXPECT_TRUE(connected.load());
    EXPECT_TRUE(serverDone.load());
}

TEST(TcpServerTest, SendWritesDataToClient) {
    const uint16_t port = reserve_free_port();
    ASSERT_NE(port, 0);

    TcpServer server("127.0.0.1", port, 0);
    std::atomic<bool> sendAccepted{ false };
    std::atomic<bool> writeComplete{ false };
    std::atomic<bool> serverDone{ false };

    server.set_connection_callback([&](const TcpConnectionPtr& conn) {
        ASSERT_NE(conn, nullptr);
        conn->send("hello");
        sendAccepted = true;
        });
    server.set_write_complete_callback([&](const TcpConnectionPtr& conn) {
        EXPECT_NE(conn, nullptr);
        writeComplete = true;
        server.stop();
        });

    std::thread serverThread([&]() {
        server.start();
        serverDone = true;
        });
    std::thread watchdog = start_watchdog(server, serverDone);

    const int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);

    const std::string response = read_with_retry(clientFd);
    EXPECT_EQ(response, "hello");
    ASSERT_EQ(::close(clientFd), 0);

    serverThread.join();
    watchdog.join();

    EXPECT_TRUE(sendAccepted.load());
    EXPECT_TRUE(writeComplete.load());
    EXPECT_TRUE(serverDone.load());
}

TEST(TcpServerTest, SendFromMessageCallbackWorksInMultiThreadMode) {
    const uint16_t port = reserve_free_port();
    ASSERT_NE(port, 0);

    TcpServer server("127.0.0.1", port, 2);
    std::atomic<bool> messageSeen{ false };
    std::atomic<bool> sendAccepted{ false };
    std::atomic<bool> writeComplete{ false };
    std::atomic<bool> serverDone{ false };

    server.set_connection_callback([](const TcpConnectionPtr& conn) {
        EXPECT_NE(conn, nullptr);
        });
    server.set_message_callback([&](const TcpConnectionPtr& conn) {
        ASSERT_NE(conn, nullptr);
        const std::string data = conn->receive();
        EXPECT_EQ(data, "ping");
        messageSeen = true;
        conn->send("pong");
        sendAccepted = true;
        });
    server.set_write_complete_callback([&](const TcpConnectionPtr& conn) {
        EXPECT_NE(conn, nullptr);
        writeComplete = true;
        server.stop();
        });

    std::thread serverThread([&]() {
        server.start();
        serverDone = true;
        });
    std::thread watchdog = start_watchdog(server, serverDone);

    const int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_EQ(::write(clientFd, "ping", 4), 4);

    const std::string response = read_with_retry(clientFd);
    EXPECT_EQ(response, "pong");
    ASSERT_EQ(::close(clientFd), 0);

    serverThread.join();
    watchdog.join();

    EXPECT_TRUE(messageSeen.load());
    EXPECT_TRUE(sendAccepted.load());
    EXPECT_TRUE(writeComplete.load());
    EXPECT_TRUE(serverDone.load());
}

TEST(TcpServerTest, ForceCloseFromConnectionCallbackClosesConnection) {
    const uint16_t port = reserve_free_port();
    ASSERT_NE(port, 0);

    TcpServer server("127.0.0.1", port, 0);
    std::atomic<bool> connected{ false };
    std::atomic<bool> closed{ false };
    std::atomic<bool> serverDone{ false };

    server.set_connection_callback([&](const TcpConnectionPtr& conn) {
        ASSERT_NE(conn, nullptr);
        connected = true;
        conn->force_close();
        });
    server.set_close_callback([&](const TcpConnectionPtr& conn) {
        EXPECT_NE(conn, nullptr);
        closed = true;
        server.stop();
        });

    std::thread serverThread([&]() {
        server.start();
        serverDone = true;
        });
    std::thread watchdog = start_watchdog(server, serverDone);

    const int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_EQ(::close(clientFd), 0);

    serverThread.join();
    watchdog.join();

    EXPECT_TRUE(connected.load());
    EXPECT_TRUE(closed.load());
    EXPECT_TRUE(serverDone.load());
}
