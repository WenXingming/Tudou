/**
 * @file main.cpp
 * @brief Tudou TCP JSON-RPC 2.0 服务端运行示例
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <iostream>
#include <spdlog/spdlog.h>
#include "tudou/rpc/JsonRpcServer.h"

int main() {
    // 设置日志等级为 info
    spdlog::set_level(spdlog::level::info);
    
    std::string ip = "127.0.0.1";
    uint16_t port = 8090;
    
    // 实例化服务端，使用 2 个工作线程（含主 Reactor 线程）
    JsonRpcServer server(ip, port, 2);
    
    // 1. 注册 add 方法：接收包含两个整数的数组，返回两数之和
    server.register_method("add", [](const nlohmann::json& params) {
        if (!params.is_array() || params.size() != 2) {
            throw std::invalid_argument("params must be an array of size 2");
        }
        return params[0].get<int>() + params[1].get<int>();
    });
    
    // 2. 注册 greet 方法：接收一个字符串参数，返回打招呼语
    server.register_method("greet", [](const nlohmann::json& params) {
        if (params.is_array() && !params.empty()) {
            return "Hello, " + params[0].get<std::string>() + "!";
        }
        return "Hello, " + params.get<std::string>() + "!";
    });
    
    spdlog::info("Starting Tudou JSON-RPC Server at {}:{}...", ip, port);
    
    // 启动监听并进入主循环（此函数会阻塞当前线程以接管 I/O 分发）
    server.start();
    
    return 0;
}
