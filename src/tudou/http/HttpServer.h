// ============================================================================
// HttpServer.h
// HTTP/HTTPS 服务器门面，把传输层事件压平成“读取、解析、执行业务、发送响应”。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// HttpServer.h
// └── HttpServer
//     ├── HttpServer(ip, port, threadNum)        # [公有] 构造服务器并绑定底层 TcpServer 回调
//     │   └── bind_tcp_callbacks()               # [私有] 绑定连接/消息/关闭事件
//     │       ├── on_connect(conn)               # [私有] 创建并登记连接级状态
//     │       │   └── create_connection_state(fd) const  # [私有] 创建 HttpContext 与可选 TLS 状态
//     │       └── on_close(conn)                 # [私有] 清理连接级状态
//     │           └── remove_connection_state(fd)  # [私有] 删除连接级状态
//     ├── HttpServer(copy)                       # [公有] 删除拷贝语义
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值
//     ├── ~HttpServer()                          # [公有] 默认析构
//     ├── start()                                # [公有] 启动底层 TCP 服务
//     ├── set_http_callback(cb)                  # [公有] 注册 HTTP 业务回调
//     ├── enable_ssl(certFile, keyFile)          # [公有] 启用 HTTPS 支持
//     ├── is_ssl_enabled() const                 # [公有] 判断 TLS 是否已启用
//     ├── get_ip() const                         # [公有] 读取监听 IP
//     ├── get_port() const                       # [公有] 读取监听端口
//     ├── get_thread_num() const                 # [公有] 读取底层 IO 线程数
//     ├── process(conn)                          # [公有] 单次消息处理总入口
//     │   ├── resolve_connection_state(conn)     # [私有] 取连接级 HttpContext 与可选 TLS 状态
//     │   │   └── find_connection_state(fd)      # [私有] 查找连接级状态
//     │   ├── read_request_payload(conn, state)  # [私有] 读取并归一化本次 HTTP 明文
//     │   ├── parse_http_request(ctx, payload)   # [私有] 推进 HTTP 解析状态机
//     │   ├── log_incomplete_request(fd)         # [私有] 记录等待更多数据
//     │   ├── reject_bad_request(conn, ctx)      # [私有] 返回 400 并重置上下文
//     │   │   ├── build_bad_request_response()   # [私有] 构建 400 响应
//     │   │   ├── send_http_response(conn, resp) # [私有] 发送响应
//     │   │   │   ├── finalize_http_response(resp) # [私有] 补齐协议头
//     │   │   │   ├── serialize_response(resp)   # [私有] 序列化响应
//     │   │   │   └── send_response(conn, state, data) # [私有] 发送明文或 TLS 密文
//     │   │   └── reset_http_context(ctx)        # [私有] 重置解析状态
//     │   └── reply_complete_request(conn, ctx)  # [私有] 执行业务回调并发送响应
//     │       ├── build_http_response(req)       # [私有] 交给业务层填充响应
//     │       │   └── build_not_found_response() # [私有] 未设置回调时回退 404
//     │       ├── send_http_response(conn, resp) # [私有] 发送响应
//     │       │   ├── finalize_http_response(resp) # [私有] 补齐协议头
//     │       │   ├── serialize_response(resp)   # [私有] 序列化响应
//     │       │   └── send_response(conn, state, data) # [私有] 发送明文或 TLS 密文
//     │       └── reset_http_context(ctx)        # [私有] 清空解析状态
// ============================================================================

#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "tudou/tcp/TcpServer.h"
#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/HttpContext.h"
#include "tudou/http/SslContext.h"
#include "tudou/http/TlsConnection.h"

// HttpServer 是 HTTP/HTTPS 协议门面，负责把连接事件翻译成稳定的请求-响应流程。
class HttpServer {
    using MessageCallback = std::function<void(const HttpRequest& req, HttpResponse& resp)>;

public:
    HttpServer(std::string ip, uint16_t port, int threadNum = 0);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    ~HttpServer() = default;

    void start();
    void set_http_callback(const MessageCallback& cb); // 设置 HTTP 业务回调。
    bool enable_ssl(const std::string& certFile, const std::string& keyFile); // 在 start 前启用 HTTPS。

    bool is_ssl_enabled() const;
    const std::string& get_ip() const;
    int get_port() const;
    int get_thread_num() const;
    void process(const std::shared_ptr<TcpConnection>& conn); // 单次消息处理总入口。

private:
    struct ConnectionState {
        std::shared_ptr<HttpContext> httpContext;
        std::shared_ptr<TlsConnection> tlsConnection;
    };

    enum class ParseState {
        Rejected,
        NeedMoreData,
        Complete
    };

    void bind_tcp_callbacks();
    void on_connect(const std::shared_ptr<TcpConnection>& conn);
    ConnectionState create_connection_state(int fd) const;
    void on_close(const std::shared_ptr<TcpConnection>& conn);
    std::string read_request_payload(const std::shared_ptr<TcpConnection>& conn,
        const ConnectionState& state) const;

    ConnectionState find_connection_state(int fd);
    ConnectionState resolve_connection_state(const std::shared_ptr<TcpConnection>& conn);
    ParseState parse_http_request(HttpContext& ctx, const std::string& payload) const; // 解析 HTTP 明文并返回当前状态。
    void log_incomplete_request(int fd) const;
    void reject_bad_request(const std::shared_ptr<TcpConnection>& conn, const ConnectionState& state, HttpContext& ctx);
    void reply_complete_request(const std::shared_ptr<TcpConnection>& conn, const ConnectionState& state, HttpContext& ctx);
    HttpResponse build_http_response(const HttpRequest& req) const; // 执行业务回调构建响应。

    HttpResponse build_bad_request_response() const;
    HttpResponse build_not_found_response() const;
    void finalize_http_response(HttpResponse& resp) const;
    std::string serialize_response(const HttpResponse& resp) const;
    void send_http_response(const std::shared_ptr<TcpConnection>& conn,
        const ConnectionState& state,
        HttpResponse resp);
    void send_response(const std::shared_ptr<TcpConnection>& conn,
        const ConnectionState& state,
        const std::string& response); // 发送明文或 TLS 密文。
    void reset_http_context(HttpContext& ctx) const;

    void remove_connection_state(int fd);

private:
    std::string ip_;                              // 服务监听 IP。
    uint16_t port_;                               // 服务监听端口。
    std::unique_ptr<TcpServer> tcpServer_;        // 底层 TCP 服务器门面。

    std::unordered_map<int, ConnectionState> connectionStates_; // 每条连接的 HTTP 解析状态与可选 TLS 状态统一同生共死。
    std::mutex contextsMutex_;                                  // 保护连接级状态映射。

    MessageCallback messageCallback_;             // 上层 HTTP 业务回调，负责把请求翻译成响应。

    std::unique_ptr<SslContext> sslContext_;      // 全局 SSL 上下文，持有证书与私钥。
};
