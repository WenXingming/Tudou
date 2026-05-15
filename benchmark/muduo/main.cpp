#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <muduo/base/Logging.h>
#include <muduo/net/Buffer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpServer.h>

namespace {

constexpr uint16_t kDefaultPort = 8080;
constexpr int kDefaultIoThreads = 0;
constexpr char kHelloBody[] = "hello world\n";
constexpr char kHelloResponse[] =
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/plain\r\n"
"Content-Length: 12\r\n"
"Connection: Keep-Alive\r\n"
"\r\n"
"hello world\n";

uint16_t parse_port(const char* text) {
    const int value = std::stoi(text);
    if (value <= 0 || value > 65535) {
        throw std::invalid_argument("port must be in (0, 65535]");
    }
    return static_cast<uint16_t>(value);
}

int parse_io_threads(const char* text) {
    const int value = std::stoi(text);
    if (value < 0) {
        throw std::invalid_argument("io_threads must be >= 0");
    }
    return value;
}

std::size_t consume_complete_requests(std::string& pending) {
    std::size_t requestCount = 0;
    std::size_t requestEnd = pending.find("\r\n\r\n");
    while (requestEnd != std::string::npos) {
        pending.erase(0, requestEnd + 4);
        ++requestCount;
        requestEnd = pending.find("\r\n\r\n");
    }
    return requestCount;
}

} // namespace

class MuduoHelloBenchmarkServer {
public:
    MuduoHelloBenchmarkServer(muduo::net::EventLoop* loop, uint16_t port, int ioThreads)
        : server_(loop, muduo::net::InetAddress(port), "MuduoHelloBenchmark") {
        server_.setConnectionCallback([this](const muduo::net::TcpConnectionPtr& conn) {
            on_connection(conn);
            });
        server_.setMessageCallback([this](const muduo::net::TcpConnectionPtr& conn,
            muduo::net::Buffer* buffer,
            muduo::Timestamp) {
                on_message(conn, buffer);
            });
        server_.setThreadNum(ioThreads);
    }

    void start() {
        server_.start();
    }

private:
    void on_connection(const muduo::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            return;
        }

        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingRequests_.erase(conn->name());
    }

    void on_message(const muduo::net::TcpConnectionPtr& conn, muduo::net::Buffer* buffer) {
        std::size_t responseCount = 0;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            std::string& pending = pendingRequests_[conn->name()];
            pending.append(buffer->retrieveAllAsString());
            responseCount = consume_complete_requests(pending);
            if (pending.empty()) {
                pendingRequests_.erase(conn->name());
            }
        }

        while (responseCount-- > 0) {
            conn->send(kHelloResponse);
        }
    }

private:
    muduo::net::TcpServer server_;
    std::unordered_map<std::string, std::string> pendingRequests_;
    std::mutex pendingMutex_;
};

int main(int argc, char* argv[]) {
    try {
        const uint16_t port = argc > 1 ? parse_port(argv[1]) : kDefaultPort;
        const int ioThreads = argc > 2 ? parse_io_threads(argv[2]) : kDefaultIoThreads;

        muduo::Logger::setLogLevel(muduo::Logger::FATAL);
        muduo::Logger::setOutput([](const char*, int) {});

        std::cout << "muduo hello benchmark listening on http://0.0.0.0:" << port
            << "/ with io_threads=" << ioThreads << std::endl;
        std::cout << "Response body: " << kHelloBody;

        muduo::net::EventLoop loop;
        MuduoHelloBenchmarkServer server(&loop, port, ioThreads);
        server.start();
        loop.loop();
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Usage: muduo-hello-benchmark [port] [io_threads]\n";
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}