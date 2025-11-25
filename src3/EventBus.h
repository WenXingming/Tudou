/*
事件总线本质就是三步：
 1. 维护一个事件表（事件名 → 回调函数列表）。
 2. 订阅就是往列表里加回调函数。
 3. 发布就是遍历回调函数，一个个调用。
*/

/// NOTE: 类型驱动型、主题驱动型...
#include <iostream>
#include <unordered_map>
#include <vector>
#include <functional>
#include <typeindex>
#include <string>

class EventBus {
public:
    // 订阅事件：模板参数就是事件类型
    // template<typename Event>
    // void subscribe(std::function<void(const Event&)> handler) {
    //     auto& vec = handlers[typeid(Event)];
    //     // 包装成统一的 void* 函数
    //     vec.push_back([handler](const void* e) {
    //         handler(*static_cast<const Event*>(e));
    //         });
    // }

    // // 发布事件
    // template<typename Event>
    // void publish(const Event& event) {
    //     auto it = handlers.find(typeid(Event));
    //     if (it != handlers.end()) {
    //         for (auto& handler : it->second) {
    //             handler(&event);
    //         }
    //     }
    // }

    template<typename Event>
    void subscribe(std::string topic, std::function<void(const Event&)> handler) {
        auto& vec = handlers[topic];
        // 包装成统一的 void* 函数
        vec.push_back([handler](const void* e) {
            handler(*static_cast<const Event*>(e));
            });
    }

    // 发布事件
    template<typename Event>
    void publish(std::string topic, const Event& event) {
        auto it = handlers.find(topic);
        if (it != handlers.end()) {
            for (auto& handler : it->second) {
                handler(&event);
            }
        }
        // it->second.clear();
    }

private:
    // 存储 (事件类型 → 回调列表)
    std::unordered_map<std::string, std::vector<std::function<void(const void*)>>> handlers;
};
