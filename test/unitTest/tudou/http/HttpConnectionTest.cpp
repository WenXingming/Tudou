#include <gtest/gtest.h>

#include "tudou/http/HttpConnection.h"

TEST(HttpConnectionTest, PlainConnectionExtractsPipelinedRequestsAndEncodesResponse) {
    HttpConnection connection;
    std::vector<HttpRequest> requests;
    std::string outboundCiphertext;

    const std::string networkData =
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n";
    EXPECT_EQ(connection.decode_requests(networkData, requests, outboundCiphertext),
        HttpConnection::ProcessResult::Success);
    EXPECT_TRUE(outboundCiphertext.empty());
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].get_path(), "/first");
    EXPECT_EQ(requests[1].get_path(), "/second");

    HttpResponse response;
    response.set_status(200, "OK");
    response.set_body("response");
    EXPECT_TRUE(connection.encode_response(response, outboundCiphertext));
    EXPECT_NE(outboundCiphertext.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(outboundCiphertext.find("\r\n\r\nresponse"), std::string::npos);
}

TEST(HttpConnectionTest, PlainConnectionReportsBadRequest) {
    HttpConnection connection;
    std::vector<HttpRequest> requests;
    std::string outboundCiphertext;

    EXPECT_EQ(connection.decode_requests("GET /broken HTTP/1.1\r\nHost localhost\r\n\r\n", requests, outboundCiphertext),
        HttpConnection::ProcessResult::BadRequest);
    EXPECT_TRUE(requests.empty());
    EXPECT_TRUE(outboundCiphertext.empty());
}

TEST(HttpConnectionTest, PlainConnectionCompletesRequestAcrossNetworkChunks) {
    HttpConnection connection;
    std::vector<HttpRequest> requests;
    std::string outboundCiphertext;

    EXPECT_EQ(connection.decode_requests("GET /health HTTP/1.1\r\nHost: local", requests, outboundCiphertext),
        HttpConnection::ProcessResult::Success);
    EXPECT_TRUE(requests.empty());

    EXPECT_EQ(connection.decode_requests("host\r\n\r\n", requests, outboundCiphertext),
        HttpConnection::ProcessResult::Success);
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().get_path(), "/health");
}
