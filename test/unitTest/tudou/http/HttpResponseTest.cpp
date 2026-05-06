#include <gtest/gtest.h>

#include <string>

#include "tudou/http/HttpResponse.h"

namespace {

std::string find_header(const HttpResponse& response, const std::string& field) {
    const HttpResponse::Headers& headers = response.get_headers();
    const auto headerIt = headers.find(field);
    if (headerIt == headers.end()) {
        return "";
    }
    return headerIt->second;
}

} // namespace

TEST(HttpResponseTest, SetHeaderOverwritesExistingValue) {
    HttpResponse response;

    response.set_header("Content-Type", "text/plain");
    response.set_header("Content-Type", "application/json");

    EXPECT_TRUE(response.has_header("Content-Type"));
    EXPECT_EQ(find_header(response, "Content-Type"), "application/json");
}

TEST(HttpResponseTest, PackageToStringSerializesStatusHeadersAndBody) {
    HttpResponse response;

    response.set_http_version("HTTP/1.1");
    response.set_status(201, "Created");
    response.set_header("Content-Type", "application/json");
    response.set_header("Content-Length", "7");
    response.set_body("created");

    const std::string packaged = response.package_to_string();

    EXPECT_NE(packaged.find("HTTP/1.1 201 Created\r\n"), std::string::npos);
    EXPECT_NE(packaged.find("Content-Type: application/json\r\n"), std::string::npos);
    EXPECT_NE(packaged.find("Content-Length: 7\r\n"), std::string::npos);
    EXPECT_NE(packaged.find("\r\n\r\ncreated"), std::string::npos);
}

TEST(HttpResponseTest, PackageToStringReflectsCloseConnectionContract) {
    HttpResponse response;

    response.set_status(200, "OK");
    response.set_close_connection(true);
    response.set_body("done");

    const std::string packaged = response.package_to_string();

    EXPECT_NE(packaged.find("Connection: close\r\n"), std::string::npos);
    EXPECT_NE(packaged.find("\r\n\r\ndone"), std::string::npos);
}