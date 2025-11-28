#include "TestHttpParser.h"

#include <iostream>

#include "../http/HttpContext.h"

namespace tudou {

    int TestHttpParser::run_all() {
        int ret = test_simple_get();
        if (ret != 0) {
            std::cout << "[TestHttpParser] test_simple_get failed, code=" << ret << std::endl;
            return ret;
        }
        std::cout << "[TestHttpParser] all tests passed" << std::endl;
        return 0;
    }

    int TestHttpParser::test_simple_get() {
        const char req[] =
            "GET /path/to/resource?name=wxm HTTP/1.1\r\n"
            "Host: localhost:8080\r\n"
            "User-Agent: TudouTest/1.0\r\n"
            "Connection: close\r\n"
            "\r\n"
            /* "hello-body" */; // Connection: close 不会有 body

        HttpContext ctx;
        size_t nparsed = 0;
        bool ok = ctx.parse(req, sizeof(req) - 1, nparsed);

        std::cout << "[TestHttpParser] ok=" << ok
            << ", nparsed=" << nparsed << std::endl;

        if (!ok || !ctx.is_complete()) {
            std::cout << "[TestHttpParser] parse failed or incomplete" << std::endl;
            return 1;
        }

        const HttpRequest& r = ctx.get_request();

        std::cout << "[TestHttpParser] method=" << r.get_method() << std::endl;
        std::cout << "[TestHttpParser] url=" << r.get_url() << std::endl;
        std::cout << "[TestHttpParser] path=" << r.get_path() << std::endl;
        std::cout << "[TestHttpParser] query=" << r.get_query() << std::endl;
        std::cout << "[TestHttpParser] host=" << r.get_header("Host") << std::endl;
        std::cout << "[TestHttpParser] body=" << r.get_body() << std::endl;

        if (r.get_method() != "GET") return 2;
        if (r.get_path() != "/path/to/resource") return 3;
        if (r.get_query() != "name=wxm") return 4;
        if (r.get_header("Host") != "localhost:8080") return 5;
        // if (r.get_body() != "hello-body") return 6;

        return 0;
    }

} // namespace tudou
