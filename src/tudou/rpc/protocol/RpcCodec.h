/**
 * @file RpcCodec.h
 * @brief Tudou 二进制 RPC 帧编解码器声明
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <string>
#include "tudou/tcp/Buffer.h"
#include "TudouProtocol.h"

namespace tudou {
namespace rpc {

class RpcCodec {
public:
    enum class DecodeResult {
        Success,    // 成功解析出一个完整的 RPC 二进制数据包
        Empty,      // 缓冲区可读字节数不足一个固定头部大小 (20 字节)
        HalfPack,   // 固定头部解析成功，但 Body 或 Meta 字节流尚未接收完整
        Error       // 魔数不匹配或版本号不对，代表发生协议损坏/非法请求
    };

    RpcCodec() = default;
    ~RpcCodec() = default;

    /**
     * @brief 将 RPC 报文编码为符合 Tudou 协议的大端二进制数据流并顺序追加进 Buffer。
     * @param buf 目标写入 Buffer
     * @param type 报文消息类型
     * @param sequenceId 会话序列 ID
     * @param metaBytes 元数据字符流
     * @param bodyBytes 消息体载荷字符流
     */
    static void encode(Buffer* buf,
                       RpcMessageType type,
                       uint64_t sequenceId,
                       const std::string& metaBytes,
                       const std::string& bodyBytes);

    /**
     * @brief 尝试从接收 Buffer 中解码出一个完整的 RPC 二进制数据帧。
     *        如果不满一个整包，本函数执行空操作且不推进 Buffer 读指针（实现非阻塞挂起）。
     * @param buf 输入数据 Buffer
     * @param outHeader 输出解码后的帧头信息
     * @param outMetaBytes 输出解码后的元数据字符流
     * @param outBodyBytes 输出解码后的消息体载荷字符流
     */
    static DecodeResult decode(Buffer* buf,
                               RpcHeader& outHeader,
                               std::string& outMetaBytes,
                               std::string& outBodyBytes);
};

} // namespace rpc
} // namespace tudou
