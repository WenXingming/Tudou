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

    EXPECT_EQ(ctx.parse(raw_request.data(), raw_request.size()), HttpContext::ParseResult::Complete);
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

    EXPECT_EQ(ctx.parse(raw_request.data(), raw_request.size()), HttpContext::ParseResult::Complete);
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

    EXPECT_EQ(ctx.parse(raw_request.data(), raw_request.size()), HttpContext::ParseResult::Complete);
    const HttpRequest& r = ctx.get_request();

    EXPECT_EQ(r.get_method(), "GET");
    EXPECT_EQ(r.get_url(), "/path/to/resource?name=wxm");
    EXPECT_EQ(r.get_path(), "/path/to/resource");
    EXPECT_EQ(r.get_query(), "name=wxm");
    EXPECT_EQ(r.get_header("Host"), "localhost:8080");
    EXPECT_TRUE(r.get_body().empty());
}

TEST(HttpContextTest, ParseRequestAcrossMultipleChunks) {
    HttpContext ctx;

    const std::string firstChunk =
        "POST /submit?source=test HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 11\r\n"
        "X-Trace: abc";
    const std::string secondChunk =
        "123\r\n"
        "\r\n"
        "hello world";

    EXPECT_EQ(ctx.parse(firstChunk.data(), firstChunk.size()), HttpContext::ParseResult::NeedMoreData);

    EXPECT_EQ(ctx.parse(secondChunk.data(), secondChunk.size()), HttpContext::ParseResult::Complete);

    const HttpRequest& req = ctx.get_request();
    EXPECT_EQ(req.get_method(), "POST");
    EXPECT_EQ(req.get_path(), "/submit");
    EXPECT_EQ(req.get_query(), "source=test");
    EXPECT_EQ(req.get_header("X-Trace"), "abc123");
    EXPECT_EQ(req.get_body(), "hello world");
}

TEST(HttpContextTest, ResetDropsPreviousRequestState) {
    HttpContext ctx;

    const std::string firstRequest =
        "GET /first?debug=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";
    const std::string secondRequest =
        "GET /second HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    ASSERT_EQ(ctx.parse(firstRequest.data(), firstRequest.size()), HttpContext::ParseResult::Complete);
    EXPECT_EQ(ctx.get_request().get_query(), "debug=1");

    ctx.reset();

    ASSERT_EQ(ctx.parse(secondRequest.data(), secondRequest.size()), HttpContext::ParseResult::Complete);
    const HttpRequest& req = ctx.get_request();
    EXPECT_EQ(req.get_path(), "/second");
    EXPECT_TRUE(req.get_query().empty());
    EXPECT_TRUE(req.get_body().empty());
}

TEST(HttpContextTest, ParseSplitRequestLinePreservesTargetAndHttpVersion) {
    HttpContext ctx;

    const std::string firstChunk = "GET /deep";
    const std::string secondChunk =
        "ly/nested?stage=1 HTTP/1.0\r\n"
        "Host: example.com\r\n";
    const std::string thirdChunk =
        "\r\n";

    EXPECT_EQ(ctx.parse(firstChunk.data(), firstChunk.size()), HttpContext::ParseResult::NeedMoreData);

    EXPECT_EQ(ctx.parse(secondChunk.data(), secondChunk.size()), HttpContext::ParseResult::NeedMoreData);

    EXPECT_EQ(ctx.parse(thirdChunk.data(), thirdChunk.size()), HttpContext::ParseResult::Complete);

    const HttpRequest& req = ctx.get_request();
    EXPECT_EQ(req.get_method(), "GET");
    EXPECT_EQ(req.get_url(), "/deeply/nested?stage=1");
    EXPECT_EQ(req.get_path(), "/deeply/nested");
    EXPECT_EQ(req.get_query(), "stage=1");
    EXPECT_EQ(req.get_version(), "HTTP/1.0");
    EXPECT_EQ(req.get_header("Host"), "example.com");
}
