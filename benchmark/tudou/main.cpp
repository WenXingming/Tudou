#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
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

thread_local std::unordered_map<ConnectionId, std::string> t_pendingRequests;

} // namespace

class TudouHelloBenchmarkServer {
public:
    TudouHelloBenchmarkServer(uint16_t port, int ioThreads)
        : server_(kListenIp, port, ioThreads) {
        server_.set_connection_callback([](ConnectionId) {});
        server_.set_message_callback([this](ConnectionId id, const std::string& data) {
            on_message(id, data);
            });
        server_.set_close_callback([this](ConnectionId id) {
            on_close(id);
            });
        server_.set_write_complete_callback([](ConnectionId) {});
    }

    void start() {
        server_.start();
    }

private:
    void on_message(ConnectionId id, const std::string& data) {
        std::size_t responseCount = 0;
        std::string& pending = t_pendingRequests[id];
        pending.append(data);
        responseCount = consume_complete_requests(pending);
        if (pending.empty()) {
            t_pendingRequests.erase(id);
        }

        while (responseCount-- > 0) {
            if (!server_.send(id, kHelloResponse)) {
                break;
            }
        }
    }

    void on_close(ConnectionId id) {
        t_pendingRequests.erase(id);
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