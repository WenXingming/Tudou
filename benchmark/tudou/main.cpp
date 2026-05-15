#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "spdlog/spdlog.h"
#include "tudou/tcp/TcpServer.h"

namespace {

constexpr char kListenIp[] = "0.0.0.0";
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

thread_local std::unordered_map<TcpConnection*, std::string> t_pendingRequests;

} // namespace

class TudouHelloBenchmarkServer {
public:
    TudouHelloBenchmarkServer(uint16_t port, int ioThreads)
        : server_(kListenIp, port, ioThreads) {
        server_.set_connection_callback([](const TcpConnectionPtr&) {});
        server_.set_message_callback([this](const TcpConnectionPtr& conn) {
            on_message(conn);
            });
        server_.set_close_callback([this](const TcpConnectionPtr& conn) {
            on_close(conn);
            });
    }

    void start() {
        server_.start();
    }

private:
    void on_message(const TcpConnectionPtr& conn) {
        const std::string data = conn ? conn->receive() : std::string();
        std::size_t responseCount = 0;
        std::string& pending = t_pendingRequests[conn.get()];
        pending.append(data);
        responseCount = consume_complete_requests(pending);
        if (pending.empty()) {
            t_pendingRequests.erase(conn.get());
        }

        while (responseCount-- > 0) {
            conn->send(kHelloResponse);
        }
    }

    void on_close(const TcpConnectionPtr& conn) {
        t_pendingRequests.erase(conn.get());
    }

private:
    TcpServer server_;
};

int main(int argc, char* argv[]) {
    try {
        const uint16_t port = argc > 1 ? parse_port(argv[1]) : kDefaultPort;
        const int ioThreads = argc > 2 ? parse_io_threads(argv[2]) : kDefaultIoThreads;

        std::cout << "Tudou hello benchmark listening on http://" << kListenIp << ':' << port
            << "/ with io_threads=" << ioThreads << std::endl;
        std::cout << "Response body: " << kHelloBody;

        spdlog::set_level(spdlog::level::off);

        TudouHelloBenchmarkServer server(port, ioThreads);
        server.start();
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Usage: tudou-hello-benchmark [port] [io_threads]\n";
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
