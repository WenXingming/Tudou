// ============================================================================
// UnifiedRpcServer 复用同一个 Protobuf Service 处理二进制 RPC 和 JSON-RPC。
// ============================================================================

#include "tudou/rpc/UnifiedRpcServer.h"

#include <stdexcept>
#include <utility>

#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>

namespace tudou {
namespace rpc {

namespace {

class SynchronousCompletion final : public google::protobuf::Closure {
public:
    void Run() override {
        completed_ = true;
    }

    bool completed() const {
        return completed_;
    }

private:
    bool completed_ = false;
};

nlohmann::json invoke_json_method(google::protobuf::Service& service,
                                  const google::protobuf::MethodDescriptor& method,
                                  const nlohmann::json& params) {
    std::unique_ptr<google::protobuf::Message> request(service.GetRequestPrototype(&method).New());

    google::protobuf::util::JsonParseOptions parseOptions;
    parseOptions.ignore_unknown_fields = true;
    const auto parseStatus = google::protobuf::util::JsonStringToMessage(
        params.is_null() ? "{}" : params.dump(), request.get(), parseOptions);
    if (!parseStatus.ok()) {
        throw std::invalid_argument("UnifiedRpcServer: Invalid JSON request: " + parseStatus.ToString());
    }

    std::unique_ptr<google::protobuf::Message> response(service.GetResponsePrototype(&method).New());

    // 两种协议共享同步 Service；完成前 request 和 response 都由当前调用栈持有。
    SynchronousCompletion completion;
    service.CallMethod(&method, nullptr, request.get(), response.get(), &completion);
    if (!completion.completed()) {
        throw std::runtime_error("UnifiedRpcServer: Asynchronous services are not supported");
    }

    google::protobuf::util::JsonPrintOptions printOptions;
    printOptions.always_print_primitive_fields = true;
    std::string responseJson;
    const auto printStatus = google::protobuf::util::MessageToJsonString(*response, &responseJson, printOptions);
    if (!printStatus.ok()) {
        throw std::runtime_error(
            "UnifiedRpcServer: Failed to encode JSON response: " + printStatus.ToString());
    }
    return nlohmann::json::parse(responseJson);
}

} // namespace

// 线程预算尽量平分；两个协议至少各自保留一个阻塞运行的 main loop。
UnifiedRpcServer::UnifiedRpcServer(const std::string& ip, uint16_t binaryPort, uint16_t jsonPort, int numThreads)
    : binaryServer_(ip, binaryPort, numThreads > 0 ? numThreads / 2 : 0),
      jsonServer_(ip, jsonPort, numThreads > 0 ? numThreads - numThreads / 2 : 0),
      binaryThread_() {
}

UnifiedRpcServer::~UnifiedRpcServer() {
    stop();
}

void UnifiedRpcServer::register_service(std::shared_ptr<google::protobuf::Service> service) {
    if (!service) {
        throw std::invalid_argument("UnifiedRpcServer: Service is null");
    }

    binaryServer_.register_service(service);

    const auto* descriptor = service->GetDescriptor();
    for (int index = 0; index < descriptor->method_count(); ++index) {
        const auto* method = descriptor->method(index);
        const std::string methodName = descriptor->full_name() + "." + method->name();
        jsonServer_.register_method(methodName, [service, method](const nlohmann::json& params) {
            return invoke_json_method(*service, *method, params);
        });
    }
}

void UnifiedRpcServer::start() {
    // 两个 Server::start() 都会阻塞，因此二进制服务使用内部线程。
    binaryThread_ = std::thread([this]() {
        binaryServer_.start();
    });
    jsonServer_.start();
}

void UnifiedRpcServer::stop() {
    binaryServer_.stop();
    jsonServer_.stop();

    if (binaryThread_.joinable()) {
        binaryThread_.join();
    }
}

uint16_t UnifiedRpcServer::get_binary_port() const {
    return binaryServer_.get_listen_port();
}

uint16_t UnifiedRpcServer::get_json_port() const {
    return jsonServer_.get_listen_port();
}

} // namespace rpc
} // namespace tudou
