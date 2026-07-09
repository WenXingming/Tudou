/**
 * @file UnifiedRpcServer.cpp
 * @brief 统一的双轨 RPC 服务端实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "UnifiedRpcServer.h"
#include "tudou/rpc/binary/BinaryRpcServer.h"
#include "tudou/rpc/json/JsonRpcServer.h"
#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <future>

namespace tudou {
namespace rpc {

UnifiedRpcServer::UnifiedRpcServer(const std::string& ip, uint16_t binaryPort, uint16_t jsonPort, int numThreads)
    : ip_(ip), binaryPort_(binaryPort), jsonPort_(jsonPort) {
    
    // 平分 I/O 线程
    int binaryThreads = numThreads > 0 ? numThreads / 2 : 0;
    int jsonThreads = numThreads > 0 ? numThreads - binaryThreads : 0;

    binaryServer_ = std::make_unique<binary::BinaryRpcServer>(ip, binaryPort, binaryThreads);
    jsonServer_ = std::make_unique<JsonRpcServer>(ip, jsonPort, jsonThreads);
}

UnifiedRpcServer::~UnifiedRpcServer() {
    stop();
}

void UnifiedRpcServer::register_service(std::shared_ptr<google::protobuf::Service> service) {
    if (!service) {
        spdlog::error("UnifiedRpcServer: Attempted to register a null service");
        return;
    }

    // 1. 注册二进制 RPC 服务
    binaryServer_->register_service(service);

    // 2. 利用 Protobuf 反射提取服务的所有接口定义，生成对应的 JSON-RPC 动态路由
    const google::protobuf::ServiceDescriptor* descriptor = service->GetDescriptor();
    std::string serviceName = descriptor->full_name();
    int methodCount = descriptor->method_count();

    for (int i = 0; i < methodCount; ++i) {
        const google::protobuf::MethodDescriptor* method = descriptor->method(i);
        
        // 生成完整的方法限定名作为 JSON-RPC 映射方法名，如 "tudou.rpc.binary.test.TestEchoService.Echo"
        std::string fullMethodName = serviceName + "." + method->name();

        jsonServer_->register_method(fullMethodName, [service, method](const nlohmann::json& params) -> nlohmann::json {
            // A. 实例化动态请求及响应 Message 对象
            std::unique_ptr<google::protobuf::Message> request(service->GetRequestPrototype(method).New());
            std::unique_ptr<google::protobuf::Message> response(service->GetResponsePrototype(method).New());

            // B. 转换 JSON 参数为 Protobuf Request
            std::string jsonStr = params.is_null() ? "{}" : params.dump();
            google::protobuf::util::JsonParseOptions parseOptions;
            parseOptions.ignore_unknown_fields = true;

            auto parseStatus = google::protobuf::util::JsonStringToMessage(jsonStr, request.get(), parseOptions);
            if (!parseStatus.ok()) {
                throw std::invalid_argument("UnifiedRpcServer: Failed to parse JSON to Protobuf request: " + parseStatus.ToString());
            }

            // C. 派发业务，并通过 promise/future 同步等待调用完成
            struct SyncClosure : public google::protobuf::Closure {
                std::promise<void> promise;
                void Run() override {
                    promise.set_value();
                }
            };

            SyncClosure doneClosure;
            auto doneFuture = doneClosure.promise.get_future();

            try {
                service->CallMethod(method, nullptr, request.get(), response.get(), &doneClosure);
                doneFuture.wait();
            }
            catch (const std::exception& e) {
                spdlog::error("UnifiedRpcServer: Exception caught in service execution: {}", e.what());
                throw;
            }

            // D. 转换 Protobuf Response 回 JSON 并返回
            std::string responseJsonStr;
            google::protobuf::util::JsonPrintOptions printOptions;
            printOptions.always_print_primitive_fields = true;

            auto printStatus = google::protobuf::util::MessageToJsonString(*response, &responseJsonStr, printOptions);
            if (!printStatus.ok()) {
                throw std::runtime_error("UnifiedRpcServer: Failed to print Protobuf response to JSON: " + printStatus.ToString());
            }

            return nlohmann::json::parse(responseJsonStr);
        });

        spdlog::info("UnifiedRpcServer: Dynamically bridged method={}", fullMethodName);
    }
}

void UnifiedRpcServer::start() {
    spdlog::info("UnifiedRpcServer: Starting dual-track servers at IP {}", ip_);

    // 1. 二进制服务端阻塞主 Reactor，所以放入后台线程运行
    binaryThread_ = std::thread([this]() {
        binaryServer_->start();
    });

    // 2. JSON 服务端占用并阻塞当前主线程以处理 Reactor 事件
    jsonServer_->start();
}

void UnifiedRpcServer::stop() {
    if (binaryServer_) {
        binaryServer_->stop();
    }
    if (jsonServer_) {
        jsonServer_->stop();
    }

    if (binaryThread_.joinable()) {
        binaryThread_.join();
    }
}

uint16_t UnifiedRpcServer::get_binary_port() const {
    return binaryServer_->get_listen_port();
}

uint16_t UnifiedRpcServer::get_json_port() const {
    return jsonPort_;
}

} // namespace rpc
} // namespace tudou
