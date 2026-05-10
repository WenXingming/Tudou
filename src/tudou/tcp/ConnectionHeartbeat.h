// ============================================================================
// ConnectionHeartbeat.h
// 通用连接空闲检测策略，附着在单个 TcpConnection 上，只负责刷新活动时间和超时断连。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// ConnectionHeartbeat.h
// └── ConnectionHeartbeat
//     ├── ConnectionHeartbeat(conn, checkInterval, idleTimeout) # [公有] 构造：绑定连接和检测参数，记录初始活动时间
//     ├── start()                                # [公有] 校验参数后启动周期定时器，定时回调 check_timeout
//     │   └── check_timeout()                    # [私有] 判定空闲超时并 force_close
//     │       └── is_timeout(now)                # [私有] 比较空闲时长与超时阈值
//     ├── stop()                                 # [公有] 取消定时器并停止检测
//     └── refresh()                              # [公有] 收到对端数据时刷新最后活动时间
// ============================================================================

#pragma once

#include <chrono>
#include <memory>

#include "Timer.h"

class EventLoop;
class TcpConnection;

class ConnectionHeartbeat : public std::enable_shared_from_this<ConnectionHeartbeat> {
public:
    ConnectionHeartbeat(const std::shared_ptr<TcpConnection>& conn,
        double checkIntervalSeconds,
        double idleTimeoutSeconds);

    void start();
    void stop();
    void refresh();

private:
    void start_in_loop();
    void stop_in_loop();
    void refresh_in_loop();
    void check_timeout();
    bool is_timeout(std::chrono::steady_clock::time_point now) const;

private:
    EventLoop* loop_; // 连接所属的 EventLoop，定时器调度和回调均在此线程执行。

    double checkIntervalSeconds_; // 空闲检测周期（秒）。
    double idleTimeoutSeconds_;   // 连接最大空闲时长（秒），超过此时间未收到对端数据则断开。
    std::chrono::steady_clock::time_point lastActiveTime_; // 最近一次被 refresh 的时间点。

    TimerId timerId_; // 当前周期定时器 ID，用于 stop() 时取消。

    std::weak_ptr<TcpConnection> conn_; // 弱引用所属连接，连接销毁后自动失效。

    bool running_; // 检测是否处于运行状态，避免重复启动或停止。
};