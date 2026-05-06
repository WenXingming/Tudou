#include <gtest/gtest.h>

#include "tudou/http/HttpRequest.h"

TEST(HttpRequestTest, ClearResetsAllProtocolFields) {
    HttpRequest request;

    request.set_method("POST");
    request.set_url("/orders?id=7");
    request.set_path("/orders");
    request.set_query("id=7");
    request.set_version("HTTP/1.1");
    request.add_header("Content-Type", "application/json");
    request.set_body("{\"ok\":true}");

    request.clear();

    EXPECT_TRUE(request.get_method().empty());
    EXPECT_TRUE(request.get_url().empty());
    EXPECT_TRUE(request.get_path().empty());
    EXPECT_TRUE(request.get_query().empty());
    EXPECT_TRUE(request.get_version().empty());
    EXPECT_TRUE(request.get_headers().empty());
    EXPECT_TRUE(request.get_body().empty());
    EXPECT_TRUE(request.get_header("Content-Type").empty());
}