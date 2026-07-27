#include <gtest/gtest.h>

#include <string>

#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/HttpRouter.h"

namespace {

HttpRequest make_request(const std::string& method, const std::string& path) {
    HttpRequest request;
    request.set_method(method);
    request.set_url(path);
    request.set_path(path);
    request.set_version("HTTP/1.1");
    return request;
}

std::string find_header(const HttpResponse& response, const std::string& field) {
    const HttpResponse::Headers& headers = response.get_headers();
    const auto headerIt = headers.find(field);
    if (headerIt == headers.end()) {
        return "";
    }
    return headerIt->second;
}

} // namespace

TEST(HttpRouterTest, DispatchUsesExactHandlerBeforePrefixFallback) {
    HttpRouter router;
    bool exactCalled = false;
    bool prefixCalled = false;

    router.add_get_route("/users", [&](const HttpRequest&, HttpResponse& response) {
        exactCalled = true;
        response.set_status(200, "OK");
        response.set_body("exact");
        });
    router.add_prefix_route("/u", [&](const HttpRequest&, HttpResponse& response) {
        prefixCalled = true;
        response.set_status(200, "OK");
        response.set_body("prefix");
        });

    HttpRequest request = make_request("GET", "/users");
    HttpResponse response;

    router.dispatch(request, response);
    EXPECT_TRUE(exactCalled);
    EXPECT_FALSE(prefixCalled);
    EXPECT_EQ(response.get_body(), "exact");
}

TEST(HttpRouterTest, DispatchFallsBackToPrefixWhenExactPathUsesAnotherMethod) {
    HttpRouter router;
    bool prefixCalled = false;

    router.add_get_route("/health", [](const HttpRequest&, HttpResponse&) {});
    router.add_post_route("/health", [](const HttpRequest&, HttpResponse&) {});
    router.add_prefix_route("/health", [&](const HttpRequest&, HttpResponse& response) {
        prefixCalled = true;
        response.set_status(200, "OK");
        });

    HttpRequest request = make_request("DELETE", "/health");
    HttpResponse response;

    router.dispatch(request, response);
    EXPECT_TRUE(prefixCalled);
    EXPECT_EQ(response.get_status_code(), 200);
}

TEST(HttpRouterTest, DispatchUsesPrefixHandlerWhenExactRouteDoesNotExist) {
    HttpRouter router;
    bool prefixCalled = false;

    router.add_prefix_route("/static", [&](const HttpRequest&, HttpResponse& response) {
        prefixCalled = true;
        response.set_status(200, "OK");
        response.set_body("asset");
        });

    HttpRequest request = make_request("GET", "/static/app.js");
    HttpResponse response;

    router.dispatch(request, response);
    EXPECT_TRUE(prefixCalled);
    EXPECT_EQ(response.get_body(), "asset");
}

TEST(HttpRouterTest, DispatchReturnsNotFoundWhenExactPathUsesAnotherMethod) {
    HttpRouter router;
    router.add_get_route("/health", [](const HttpRequest&, HttpResponse&) {});

    HttpRequest request = make_request("DELETE", "/health");
    HttpResponse response;

    router.dispatch(request, response);
    EXPECT_EQ(response.get_status_code(), 404);
}

TEST(HttpRouterTest, DispatchUsesFirstRegisteredPrefixHandlerWhenMultiplePrefixesMatch) {
    HttpRouter router;
    std::string matchedHandler;

    router.add_prefix_route("/static", [&](const HttpRequest&, HttpResponse& response) {
        matchedHandler = "first";
        response.set_status(200, "OK");
        response.set_body("first-prefix");
        });
    router.add_prefix_route("/static/app", [&](const HttpRequest&, HttpResponse& response) {
        matchedHandler = "second";
        response.set_status(200, "OK");
        response.set_body("second-prefix");
        });

    HttpRequest request = make_request("GET", "/static/app.js");
    HttpResponse response;

    router.dispatch(request, response);
    EXPECT_EQ(matchedHandler, "first");
    EXPECT_EQ(response.get_body(), "first-prefix");
}

TEST(HttpRouterTest, DispatchReturnsDefaultNotFoundResponseWhenNoRouteMatches) {
    HttpRouter router;

    HttpRequest request = make_request("GET", "/missing");
    HttpResponse response;

    router.dispatch(request, response);
    EXPECT_EQ(response.get_http_version(), "HTTP/1.1");
    EXPECT_EQ(response.get_status_code(), 404);
    EXPECT_EQ(response.get_status_message(), "Not Found");
    EXPECT_EQ(response.get_body(), "Not Found");
    EXPECT_EQ(find_header(response, "Content-Type"), "text/plain");
    EXPECT_EQ(find_header(response, "Connection"), "close");
}
