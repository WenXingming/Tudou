// ============================================================================
// Router 将一次逻辑请求映射为具体的 Protobuf Service::CallMethod。
// ============================================================================

#include "tudou/rpc/binary/Router.h"

#include <stdexcept>
#include <utility>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

namespace tudou {
namespace rpc {
namespace binary {

namespace {

class SynchronousCompletion : public google::protobuf::Closure {
public:
    SynchronousCompletion()
        : completed_(false) {
    }

    void Run() override {
        completed_ = true;
    }

    bool completed() const {
        return completed_;
    }

private:
    bool completed_;
};

} // namespace

Router::Router()
    : services_() {
}

Router::~Router() = default;

void Router::register_service(std::shared_ptr<google::protobuf::Service> service) {
    if (!service) {
        throw std::invalid_argument("Router: Service is null");
    }
    const google::protobuf::ServiceDescriptor* serviceDescriptor = service->GetDescriptor();
    const std::string serviceName = serviceDescriptor->full_name();
    services_[serviceName] = std::move(service);
}

std::string Router::dispatch(const Request& request) const {
    const auto serviceIt = services_.find(request.serviceName);
    if (serviceIt == services_.end()) {
        throw std::invalid_argument("Router: Service not found: " + request.serviceName);
    }
    const auto& service = serviceIt->second;

    const auto* serviceDescriptor = service->GetDescriptor();
    const auto* method = serviceDescriptor->FindMethodByName(request.methodName);
    if (method == nullptr) {
        throw std::invalid_argument("Router: Method not found: " + request.serviceName + "." + request.methodName);
    }

    std::unique_ptr<google::protobuf::Message> protobufRequest(service->GetRequestPrototype(method).New());
    if (!protobufRequest->ParseFromString(request.body)) {
        throw std::invalid_argument("Router: Invalid request body: " + request.serviceName + "." + request.methodName);
    }

    std::unique_ptr<google::protobuf::Message> protobufResponse(service->GetResponsePrototype(method).New());

    SynchronousCompletion completion;
    service->CallMethod(method, nullptr, protobufRequest.get(), protobufResponse.get(), &completion);
    if (!completion.completed()) {
        throw std::runtime_error("Router: Asynchronous services are not supported");
    }

    return protobufResponse->SerializeAsString();
}

} // namespace binary
} // namespace rpc
} // namespace tudou
