#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unistd.h>

#include "base/ScopedFd.h"
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

TEST(HttpResponseTest, FileBodySerializesOnlyHeaders) {
    HttpResponse response;
    auto file = std::make_shared<ScopedFd>(::dup(STDOUT_FILENO));
    ASSERT_TRUE(file->valid());

    response.set_status(200, "OK");
    response.set_header("Content-Length", "12");
    response.set_body("do-not-serialize");
    response.set_file_body(file, 12, 3);

    const std::string packaged = response.package_to_string();

    EXPECT_TRUE(response.has_file_body());
    EXPECT_EQ(response.get_file_fd(), file->fd());
    EXPECT_EQ(response.get_file_size(), 12U);
    EXPECT_EQ(response.get_file_offset(), 3U);
    EXPECT_TRUE(response.get_body().empty());
    EXPECT_NE(packaged.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(packaged.find("Content-Length: 12\r\n"), std::string::npos);
    EXPECT_NE(packaged.find("\r\n\r\n"), std::string::npos);
    EXPECT_EQ(packaged.find("do-not-serialize"), std::string::npos);
}

TEST(HttpResponseTest, PlainTextFactoryBuildsDefaultErrorShape) {
    HttpResponse response = HttpResponse::plain_text(404, "Not Found", "Not Found");

    EXPECT_EQ(response.get_http_version(), "HTTP/1.1");
    EXPECT_EQ(response.get_status_code(), 404);
    EXPECT_EQ(response.get_status_message(), "Not Found");
    EXPECT_EQ(response.get_body(), "Not Found");
    EXPECT_EQ(find_header(response, "Content-Type"), "text/plain");
    EXPECT_EQ(find_header(response, "Content-Length"), std::to_string(response.get_body().size()));
    EXPECT_TRUE(response.get_close_connection());
}
