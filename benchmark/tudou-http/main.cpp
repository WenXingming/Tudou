#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "spdlog/spdlog.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/HttpServer.h"

namespace {

constexpr char kListenIp[] = "0.0.0.0";
constexpr uint16_t kDefaultPort = 8080;
constexpr int kDefaultIoThreads = 0;
constexpr char kHelloBody[] = "hello world\n";

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

} // namespace

class TudouHttpBenchmarkServer {
public:
    TudouHttpBenchmarkServer(uint16_t port, int ioThreads)
        : server_(kListenIp, port, ioThreads) {
        server_.add_get_route("/", [](const HttpRequest&, HttpResponse& resp) {
            resp.set_http_version("HTTP/1.1");
            resp.set_status(200, "OK");
            resp.set_header("Content-Type", "text/plain");
            resp.set_body(kHelloBody);
            });
    }

    void start() {
        server_.start();
    }

private:
    HttpServer server_;
};

int main(int argc, char* argv[]) {
    try {
        const uint16_t port = argc > 1 ? parse_port(argv[1]) : kDefaultPort;
        const int ioThreads = argc > 2 ? parse_io_threads(argv[2]) : kDefaultIoThreads;

        std::cout << "Tudou HTTP benchmark listening on http://" << kListenIp << ':' << port
            << "/ with io_threads=" << ioThreads << std::endl;
        std::cout << "Response body: " << kHelloBody;

        spdlog::set_level(spdlog::level::off);

        TudouHttpBenchmarkServer server(port, ioThreads);
        server.start();
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Usage: tudou-http-benchmark [port] [io_threads]\n";
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}