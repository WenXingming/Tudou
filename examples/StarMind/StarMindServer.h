#pragma once

#include <memory>
#include <string>

#include "StarMindConfig.h"

class HttpRequest;
class HttpResponse;
class HttpServer;

class StarMindServer {
public:
    explicit StarMindServer(StarMindServerConfig cfg);
    ~StarMindServer();

    void start();

private:
    void init();

private:
    struct StarMindState;

private:
    StarMindServerConfig cfg_;
    std::unique_ptr<StarMindState> state_;
    std::unique_ptr<HttpServer> httpServer_;
};
