/**
 * @file StarMindConfig.h
 * @brief StarMind 配置数据定义
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <cstdint>
#include <string>

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

    // Paths resolved by ConfigLoader
    std::string serverRoot;   // ends with '/'
    std::string configPath;   // {serverRoot}conf/server.conf
    std::string logPath;      // {serverRoot}log/server.log
};
