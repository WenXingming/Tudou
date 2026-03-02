/**
 * @file https_test.cpp
 * @brief HTTPS 功能快速测试程序
 *
 * 启动一个简单的 HTTPS 服务器，响应 "Hello, HTTPS!" 用于验证 TLS 集成是否正常工作。
 * 使用方法：
 *   编译后运行，然后用 curl -k https://127.0.0.1:8443/ 测试
 */

#include <iostream>
#include <string>
#include "tudou/http/HttpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "spdlog/spdlog.h"

int main() {
    spdlog::set_level(spdlog::level::debug);

    HttpServer server("0.0.0.0", 8443, 0);

    // 启用 SSL。相对路径是相对于进程的当前工作目录，而不是源代码目录或者二进制可执行文件所在目录，因此需要确保运行时的工作目录正确，或者使用绝对路径。
    if (!server.enable_ssl("certs/test-cert.pem", "certs/test-key.pem")) {
        std::cerr << "Failed to enable SSL" << std::endl;
        return 1;
    }

    // 设置简单的回调
    server.set_http_callback([](const HttpRequest& req, HttpResponse& resp) {
        resp.set_http_version("HTTP/1.1");
        resp.set_status(200, "OK");
        resp.add_header("Content-Type", "text/plain");
        resp.set_body("Hello, HTTPS!\n");
        });

    std::cout << "HTTPS server starting on https://0.0.0.0:8443/" << std::endl;
    server.start();

    return 0;
}
