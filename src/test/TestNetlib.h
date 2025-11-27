/**
 * @file TestNetlib.h
 * @brief 单元测试。测试底层网络库基本功能，还没有封装上层 TcpServer
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou.git
 */

#include <iostream>
#include <memory>

class EventLoop;
class Channel;
class TestNetlib {
private:
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<Channel> channel;

public:
    TestNetlib();
    ~TestNetlib();

    void start();
};