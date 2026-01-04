#include <gtest/gtest.h>
#include <string>

#include "tudou/http/HttpContext.h"
#include "tudou/http/HttpRequest.h"

// 测试 HttpContext 能够正确解析一个简单的 GET 请求
TEST(HttpContextTest, ParseSimpleGetRequest) {
    HttpContext ctx;

    const std::string raw_request =
        "GET /hello?name=world HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: TudouTest\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "Hello";

    size_t nparsed = 0;
    bool ok = ctx.parse(raw_request.data(), raw_request.size(), nparsed);

    EXPECT_TRUE(ok);
    EXPECT_EQ(nparsed, raw_request.size());
    EXPECT_TRUE(ctx.is_complete());

    const HttpRequest& req = ctx.get_request();

    EXPECT_EQ(req.get_method(), "GET");
    EXPECT_EQ(req.get_url(), "/hello?name=world");
    EXPECT_EQ(req.get_path(), "/hello");
    EXPECT_EQ(req.get_query(), "name=world");
    EXPECT_EQ(req.get_version(), "HTTP/1.1");

    EXPECT_EQ(req.get_header("Host"), "example.com");
    EXPECT_EQ(req.get_header("User-Agent"), "TudouTest");
    EXPECT_EQ(req.get_header("Content-Length"), "5");

    EXPECT_EQ(req.get_body(), "Hello");
}

// 测试没有 query 的 URL
TEST(HttpContextTest, ParseGetWithoutQuery) {
    HttpContext ctx;

    const std::string raw_request =
        "GET /index.html HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    size_t nparsed = 0;
    bool ok = ctx.parse(raw_request.data(), raw_request.size(), nparsed);

    EXPECT_TRUE(ok);
    EXPECT_EQ(nparsed, raw_request.size());
    EXPECT_TRUE(ctx.is_complete());

    const HttpRequest& req = ctx.get_request();

    EXPECT_EQ(req.get_method(), "GET");
    EXPECT_EQ(req.get_url(), "/index.html");
    EXPECT_EQ(req.get_path(), "/index.html");
    EXPECT_TRUE(req.get_query().empty());
    EXPECT_EQ(req.get_version(), "HTTP/1.1");

    EXPECT_EQ(req.get_header("Host"), "localhost");
    EXPECT_TRUE(req.get_body().empty());
}

// 从原先 TestHttpParser 迁移而来的测试用例：解析带路径和 query 的简单 GET 请求。curl -v http://127.0.0.1:8080/ -o /dev/null
TEST(HttpContextTest, ParseSimpleGet_FromLegacyTestHttpParser) {
    HttpContext ctx;

    const std::string raw_request =
        "GET /path/to/resource?name=wxm HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: TudouTest/1.0\r\n"
        "Connection: close\r\n"
        "\r\n";

    size_t nparsed = 0;
    bool ok = ctx.parse(raw_request.data(), raw_request.size(), nparsed);

    EXPECT_TRUE(ok);
    EXPECT_EQ(nparsed, raw_request.size());
    EXPECT_TRUE(ctx.is_complete());

    const HttpRequest& r = ctx.get_request();

    EXPECT_EQ(r.get_method(), "GET");
    EXPECT_EQ(r.get_url(), "/path/to/resource?name=wxm");
    EXPECT_EQ(r.get_path(), "/path/to/resource");
    EXPECT_EQ(r.get_query(), "name=wxm");
    EXPECT_EQ(r.get_header("Host"), "localhost:8080");
    EXPECT_TRUE(r.get_body().empty());
}
