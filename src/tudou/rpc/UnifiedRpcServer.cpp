// ============================================================================
// UnifiedRpcServer 的注册流程是：注册二进制服务、注册桥接路由、暴露 JSON 方法。
// ============================================================================

#include "tudou/rpc/UnifiedRpcServer.h"

#include <stdexcept>
#include <utility>

#include <google/protobuf/util/json_util.h>

namespace tudou {
namespace rpc {

UnifiedRpcServer::UnifiedRpcServer(
    const std::string& ip,
    uint16_t binaryPort,
    uint16_t jsonPort,
    int numThreads)
    : binaryServer_(
          ip,
          binaryPort,
          numThreads > 0 ? numThreads / 2 : 0),
      jsonServer_(
          ip,
          jsonPort,
          numThreads > 0 ? numThreads - numThreads / 2 : 0),
      jsonBridgeRouter_(),
      binaryThread_() {
}

UnifiedRpcServer::~UnifiedRpcServer() {
    stop();
}

void UnifiedRpcServer::register_service(
    std::shared_ptr<google::protobuf::Service> service) {
    if (!service) {
        throw std::invalid_argument("UnifiedRpcServer: Service is null");
    }

    binaryServer_.register_service(service);
    jsonBridgeRouter_.register_service(service);

    const auto* descriptor = service->GetDescriptor();
    for (int index = 0; index < descriptor->method_count(); ++index) {
        register_json_method(service, descriptor->method(index));
    }
}

void UnifiedRpcServer::start() {
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

void UnifiedRpcServer::register_json_method(
    const std::shared_ptr<google::protobuf::Service>& service,
    const google::protobuf::MethodDescriptor* method) {
    const std::string methodName =
        service->GetDescriptor()->full_name() + "." + method->name();
    jsonServer_.register_method(
        methodName,
        [this, service, method](const nlohmann::json& params) {
            return invoke_json_method(*service, *method, params);
        });
}

nlohmann::json UnifiedRpcServer::invoke_json_method(
    google::protobuf::Service& service,
    const google::protobuf::MethodDescriptor& method,
    const nlohmann::json& params) const {
    std::unique_ptr<google::protobuf::Message> request(
        service.GetRequestPrototype(&method).New());

    google::protobuf::util::JsonParseOptions parseOptions;
    parseOptions.ignore_unknown_fields = true;
    const auto parseStatus = google::protobuf::util::JsonStringToMessage(
        params.is_null() ? "{}" : params.dump(),
        request.get(),
        parseOptions);
    if (!parseStatus.ok()) {
        throw std::invalid_argument(
            "UnifiedRpcServer: Invalid JSON request: " + parseStatus.ToString());
    }

    const binary::Request rpcRequest(
        service.GetDescriptor()->full_name(),
        method.name(),
        request->SerializeAsString());
    const std::string responseBody = jsonBridgeRouter_.dispatch(rpcRequest);

    std::unique_ptr<google::protobuf::Message> response(
        service.GetResponsePrototype(&method).New());
    if (!response->ParseFromString(responseBody)) {
        throw std::runtime_error("UnifiedRpcServer: Invalid Protobuf response");
    }

    google::protobuf::util::JsonPrintOptions printOptions;
    printOptions.always_print_primitive_fields = true;
    std::string responseJson;
    const auto printStatus = google::protobuf::util::MessageToJsonString(
        *response,
        &responseJson,
        printOptions);
    if (!printStatus.ok()) {
        throw std::runtime_error(
            "UnifiedRpcServer: Failed to encode JSON response: "
            + printStatus.ToString());
    }
    return nlohmann::json::parse(responseJson);
}

} // namespace rpc
} // namespace tudou
