// ============================================================================
// Router 根据 service + method 找到 Protobuf Service 并执行一次调用。
// 它负责动态消息类型的反序列化和序列化，不处理网络、帧或连接生命周期。
// ============================================================================

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <google/protobuf/service.h>

#include "tudou/rpc/binary/Request.h"

namespace tudou {
namespace rpc {
namespace binary {

class Router {
public:
    Router();
    ~Router();

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    // Service 应在服务器启动前注册，并在 CallMethod 返回前完成请求。
    void register_service(std::shared_ptr<google::protobuf::Service> service);
    std::string dispatch(const Request& request) const;

private:
    std::unordered_map<std::string, std::shared_ptr<google::protobuf::Service>> services_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
