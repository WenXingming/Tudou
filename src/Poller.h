/**
 * @file Poller.h
 * @brief 作为多路事件分发器, 是 IO 服用核心模块 (epoll_create1 / epoll_wait / epoll_ctl)。类似于注册中心（epoll、channels）
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#pragma once
#include <vector>
#include <unordered_map>
#include <memory>
#include "../base/NonCopyable.h"
#include "../base/Timestamp.h"

class EventLoop;
class Channel;
class Poller : public NonCopyable {
private:
    // 先调用基类构造函数。loop 派生类并没有用到，所以在基类里面是 private
    EventLoop* ownLoop; // poller 所属的 EventLoop。子类 poller 暂时无需依赖，所以 private

protected:
    // std::unordered_map<int, Channel*> channels; // 子类需要使用，所以 protected

public:
    Poller(EventLoop* _loop) : ownLoop(_loop) {}; // 构造函数不能是 virtual。只有当对象被构造完成，vptr 指针被正确设置后，虚函数的机制才能正常工作。
    virtual ~Poller() {}; // 析构函数必须是 virtual。如果析构函数不是 virtual，当通过一个基类指针删除一个派生类对象时，只会调用基类的析构函数，而不会调用派生类的析构函数。这会导致资源泄漏。


    virtual std::vector<Channel*> poll(int timeoutMs) = 0; // @brief  核心函数

    virtual void update_channel(Channel* channel) = 0;
    virtual void remove_channel(Channel* channel) = 0;

    virtual bool has_channel(const Channel* Channel) const;


    static Poller* new_default_poller(EventLoop* loop); // @brief EventLoop 通过该接口获取 IO 多路服用对象
};
