#pragma once

#include <cstdint>
#include <memory>
#include <string>

class HttpRequest;
class HttpResponse;
class HttpServer;
class Router;

struct StarMindServerConfig {
    std::string ip = "0.0.0.0";
    uint16_t port = 8090;
    int threadNum = 0;

    // Static web
    std::string webRoot = "";   // absolute or relative to serverRoot resolved by main
    std::string indexFile = "login.html";

    // Auth
    bool authEnabled = true;
    std::string authUser = "admin";
    std::string authPassword = "admin";
    int authTokenTtlSeconds = 86400;

    // LLM
    // provider: mock | openai_compat
    std::string llmProvider = "openai_compat";
    std::string llmApiBase = "https://api.deepseek.com/v1";
    std::string llmApiKey = "";
    std::string llmModel = "deepseek-chat";
    int llmTimeoutSeconds = 60;

    std::string llmSystemPrompt = "You are StarMind, a helpful assistant.";
    int llmMaxHistoryMessages = 20;
};

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
