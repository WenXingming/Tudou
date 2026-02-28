#pragma once

#include <memory>
#include <string>

#include "StarMindConfig.h"

class HttpRequest;
class HttpResponse;
class HttpServer;
class Router;

class StarMindServer {
public:
    explicit StarMindServer(StarMindServerConfig cfg);
    ~StarMindServer();

    void start();

private:
    void init();
    void on_http_request(const HttpRequest& req, HttpResponse& resp);

private:
    struct StarMindState;

private:
    StarMindServerConfig cfg_;
    std::unique_ptr<StarMindState> state_;
    std::unique_ptr<HttpServer> httpServer_;
    std::unique_ptr<Router> router_;
};
