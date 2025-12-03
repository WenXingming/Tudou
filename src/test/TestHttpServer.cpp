#include "TestHttpServer.h"

#include <fstream>
#include <sstream>

#include "../base/Log.h"
#include "../http/HttpServer.h"


TestHttpServer::TestHttpServer(int port, const std::string& html_path)
    : port_(port),
    html_path_(html_path),
    listen_addr_(port),
    http_server_(&loop_, listen_addr_) {

    http_server_.set_http_callback(
        [this](const HttpRequest& req, HttpResponse& resp) {
            on_http_request(req, resp);
        });
}

void TestHttpServer::start() {
    http_server_.start();
    loop_.loop();
}

void TestHttpServer::on_http_request(const HttpRequest& req, HttpResponse& resp) {
    // LOG::LOG_INFO("[TestHttpServer] method=%s, path=%s", req.get_method().c_str(), req.get_path().c_str());
    // 目前只处理 GET / 和 /index.html，返回首页
    if (req.get_method() == "GET" &&
        (req.get_path() == "/" || req.get_path() == "/index.html")) {
        // LOG::LOG_INFO("[TestHttpServer] Serving homepage: %s", html_path_.c_str());

        std::ifstream ifs(html_path_);
        if (!ifs) {
            // LOG::LOG_ERROR("[TestHttpServer] Failed to open html file: %s", html_path_.c_str());
            resp.set_status(500, "Internal Server Error");
            resp.set_body("Failed to open html file");
            resp.add_header("Content-Type", "text/plain; charset=utf-8");
            resp.set_close_connection(true);
            return;
        }

        std::stringstream ss;
        ss << ifs.rdbuf();

        resp.set_status(200, "OK");
        resp.set_body(ss.str());
        resp.add_header("Content-Type", "text/html; charset=utf-8");
        resp.set_close_connection(true); // 先用短连接，简单一点
    }
    else {
        // LOG::LOG_INFO("[TestHttpServer] 404 Not Found: %s",
        //     req.get_path().c_str());
        resp.set_status(404, "Not Found");
        resp.set_body("Not Found");
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_close_connection(true);
    }
}
