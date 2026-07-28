// ============================================================================
// Connection 保存一条 RPC 字节流尚未组成完整帧的部分。
// 负责循环 decode，它只解决 TCP 半包和粘包，不拥有 Socket，也不执行路由或业务回调。
// ============================================================================

#pragma once

#include <string>
#include <vector>

#include "tudou/rpc/binary/Frame.h"
#include "tudou/tcp/Buffer.h"

namespace tudou {
namespace rpc {
namespace binary {

class Connection {
public:
    Connection();
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool decode(const std::string& data, std::vector<Frame>& frames);

private:
    Buffer inputBuffer_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
