// ============================================================================
// ConnectionHeartbeat 负责单个 TcpConnection 的空闲检测和超时关闭。
// 它通过 EventLoop 定时检查活动时间，不拥有 TcpConnection。
// ============================================================================

#pragma once

#include <chrono>
#include <memory>

#include "tudou/timer/Timer.h"

class EventLoop;
class TcpConnection;

class ConnectionHeartbeat : public std::enable_shared_from_this<ConnectionHeartbeat> {
public:
    ConnectionHeartbeat(const std::shared_ptr<TcpConnection>& conn,
        double checkIntervalSeconds,
        double idleTimeoutSeconds);

    // 启停检测并刷新连接活动时间。
    void start();
    void stop();
    void refresh();

private:
    void check_timeout();
    bool is_timeout(std::chrono::steady_clock::time_point now) const;

private:
    EventLoop* loop_;                                           // 连接所属的 EventLoop，定时器调度和回调均在此线程执行。

    double checkIntervalSeconds_;                               // 空闲检测周期（秒）。
    double idleTimeoutSeconds_;                                 // 连接最大空闲时长（秒），超过此时间未收到对端数据则断开。
    std::chrono::steady_clock::time_point lastActiveTime_;      // 最近一次被 refresh 的时间点。

    TimerId timerId_;                                           // 当前周期定时器 ID；有效表示检测已启动。

    std::weak_ptr<TcpConnection> conn_;                         // 弱引用所属连接，连接销毁后自动失效。
};
