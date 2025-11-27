#include <iostream>
#include <thread>
#include <chrono>
#include "NonCopyable.h"
#include "Log.h"
#include "Timestamp.h"
#include "InetAddress.h"

/// @brief 测试 NonCopyable.h
class Parent : NonCopyable {
private:
    int a;
public:
    explicit Parent(int _a) : a(_a) {}
    ~Parent() {}

    void print() {
        std::cout << "value of a is: " << a << std::endl;
    }
};

void test_noncopyable() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "test_noncopyable running...\n";

    Parent object(10);
    object.print();

    // 无法引用 函数 "Parent::Parent(const Parent &)" (已隐式声明) -- 它是已删除的函数C/C++(1776)
    // Parent otherObject(object);

    std::cout << "test_noncopyable success.\n";
}


/// @brief 测试 Logger.h、Log.h
void test_logger() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "test_logger running...\n";

    LOG::LOG_INFO("info log msg, val is: %d", 100);
    LOG::LOG_ERROR("error log, msg is: %s", "hello world");
    LOG::LOG_FATAL("fatal log, val is: %f", 100.00);
    LOG::LOG_DEBUG("debug log");

    std::cout << "test_logger success.\n";
}


/// @brief 测试 Timestamp.h
void test_timestamp() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "test_timestamp running...\n";

    std::cout << Timestamp::now().timestamp_to_string() << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    std::cout << Timestamp::now().timestamp_to_string() << std::endl;

    std::cout << "test_timestamp success.\n";
}


/// @brief 测试 InetAddress
void test_inet_address() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "test_inet_address running...\n";

    InetAddress inetAddress = InetAddress(8080);
    std::cout << "ip: " << inetAddress.get_ip() << std::endl;
    std::cout << "port: " << inetAddress.get_port() << std::endl;
    std::cout << "ip:port" << inetAddress.get_ip_port() << std::endl;

    std::cout << "test_inet_address success.\n";
}

// int main() {
//     test_noncopyable();
//     test_logger();
//     test_timestamp();
//     test_inet_address();
// }