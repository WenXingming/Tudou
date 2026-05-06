#include <gtest/gtest.h>

#include <string>

#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/router/Router.h"

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

TEST(RouterTest, DispatchUsesExactHandlerBeforePrefixFallback) {
    Router router;
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

    EXPECT_EQ(router.dispatch(request, response), DispatchResult::Matched);
    EXPECT_TRUE(exactCalled);
    EXPECT_FALSE(prefixCalled);
    EXPECT_EQ(response.get_body(), "exact");
}

TEST(RouterTest, DispatchReturnsMethodNotAllowedBeforePrefixFallback) {
    Router router;
    bool prefixCalled = false;

    router.add_get_route("/health", [](const HttpRequest&, HttpResponse&) {});
    router.add_post_route("/health", [](const HttpRequest&, HttpResponse&) {});
    router.add_prefix_route("/health", [&](const HttpRequest&, HttpResponse& response) {
        prefixCalled = true;
        response.set_status(200, "OK");
        });

    HttpRequest request = make_request("DELETE", "/health");
    HttpResponse response;

    EXPECT_EQ(router.dispatch(request, response), DispatchResult::MethodNotAllowed);
    EXPECT_FALSE(prefixCalled);
    EXPECT_EQ(response.get_status_code(), 405);
    EXPECT_EQ(response.get_status_message(), "Method Not Allowed");
    EXPECT_EQ(response.get_body(), "Method Not Allowed");
    EXPECT_EQ(find_header(response, "Allow"), "GET, POST");
    EXPECT_EQ(find_header(response, "Content-Type"), "text/plain");
    EXPECT_EQ(find_header(response, "Content-Length"), std::to_string(response.get_body().size()));
    EXPECT_TRUE(response.get_close_connection());
}

TEST(RouterTest, DispatchUsesPrefixHandlerWhenExactRouteDoesNotExist) {
    Router router;
    bool prefixCalled = false;

    router.add_prefix_route("/static", [&](const HttpRequest&, HttpResponse& response) {
        prefixCalled = true;
        response.set_status(200, "OK");
        response.set_body("asset");
        });

    HttpRequest request = make_request("GET", "/static/app.js");
    HttpResponse response;

    EXPECT_EQ(router.dispatch(request, response), DispatchResult::Matched);
    EXPECT_TRUE(prefixCalled);
    EXPECT_EQ(response.get_body(), "asset");
}

TEST(RouterTest, DispatchUsesFirstRegisteredPrefixHandlerWhenMultiplePrefixesMatch) {
    Router router;
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

    EXPECT_EQ(router.dispatch(request, response), DispatchResult::Matched);
    EXPECT_EQ(matchedHandler, "first");
    EXPECT_EQ(response.get_body(), "first-prefix");
}

TEST(RouterTest, DispatchReturnsDefaultNotFoundResponseWhenNoRouteMatches) {
    Router router;

    HttpRequest request = make_request("GET", "/missing");
    HttpResponse response;

    EXPECT_EQ(router.dispatch(request, response), DispatchResult::NotFound);
    EXPECT_EQ(response.get_http_version(), "HTTP/1.1");
    EXPECT_EQ(response.get_status_code(), 404);
    EXPECT_EQ(response.get_status_message(), "Not Found");
    EXPECT_EQ(response.get_body(), "Not Found");
    EXPECT_EQ(find_header(response, "Content-Type"), "text/plain");
    EXPECT_EQ(find_header(response, "Content-Length"), std::to_string(response.get_body().size()));
    EXPECT_TRUE(response.get_close_connection());
}

TEST(RouterTest, DispatchUsesCustomNotFoundHandlerWhenProvided) {
    Router router;
    bool notFoundCalled = false;

    router.set_not_found_handler([&](const HttpRequest&, HttpResponse& response) {
        notFoundCalled = true;
        response.set_status(410, "Gone");
        response.set_body("custom-not-found");
        });

    HttpRequest request = make_request("GET", "/missing");
    HttpResponse response;

    EXPECT_EQ(router.dispatch(request, response), DispatchResult::NotFound);
    EXPECT_TRUE(notFoundCalled);
    EXPECT_EQ(response.get_status_code(), 410);
    EXPECT_EQ(response.get_body(), "custom-not-found");
}

TEST(RouterTest, DispatchUsesCustomMethodNotAllowedHandlerWhenProvided) {
    Router router;
    bool customHandlerCalled = false;

    router.add_get_route("/health", [](const HttpRequest&, HttpResponse&) {});
    router.set_method_not_allowed_handler([&](const HttpRequest&, HttpResponse& response) {
        customHandlerCalled = true;
        response.set_status(499, "Custom");
        response.set_body("custom-method-not-allowed");
        });

    HttpRequest request = make_request("PATCH", "/health");
    HttpResponse response;

    EXPECT_EQ(router.dispatch(request, response), DispatchResult::MethodNotAllowed);
    EXPECT_TRUE(customHandlerCalled);
    EXPECT_EQ(response.get_status_code(), 499);
    EXPECT_EQ(response.get_body(), "custom-method-not-allowed");
    EXPECT_EQ(find_header(response, "Allow"), "");
}