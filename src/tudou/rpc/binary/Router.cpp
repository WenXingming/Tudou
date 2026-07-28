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

// Router 在栈上持有请求和响应消息，因此 Service 必须在 CallMethod 返回前调用 done->Run()。
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
    // 先定位已注册 Service，再利用 Descriptor 找到 protoc 生成的方法入口。
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

    // Prototype 根据方法描述符创建真实的业务消息类型，Router 不需要知道具体 .proto 类。
    std::unique_ptr<google::protobuf::Message> protobufRequest(service->GetRequestPrototype(method).New());
    if (!protobufRequest->ParseFromString(request.body)) {
        throw std::invalid_argument("Router: Invalid request body: " + request.serviceName + "." + request.methodName);
    }

    std::unique_ptr<google::protobuf::Message> protobufResponse(service->GetResponsePrototype(method).New());

    // CallMethod 最终进入用户实现的 Service 方法；完成标记明确拒绝异步 Service。
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
