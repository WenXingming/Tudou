/**
 * @file Timestamp.h
 * @brief 时间戳类，主要是方便日志打印。使用：Timestamp::now().timestamp_to_string()
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#pragma once
#include <chrono>
#include <string>
#include <sstream>
#include <iomanip>

 /// @details Use: Timestamp::now().timestamp_to_string()
class Timestamp {
private:
    std::chrono::_V2::system_clock::time_point timePoint;

public:
    explicit Timestamp(std::chrono::_V2::system_clock::time_point _timePoint = std::chrono::system_clock::now())
        : timePoint(_timePoint) {}
    ~Timestamp() {}

    static Timestamp now() {
        auto now = std::chrono::system_clock::now();
        return Timestamp(now);
    }

    std::string timestamp_to_string() {
        // 转换为 time_t 类型
        std::time_t now_time = std::chrono::system_clock::to_time_t(timePoint);
        // 转换为本地时间。非线程安全
        std::tm* local_time = std::localtime(&now_time);
        // 格式化输出
        std::stringstream ss;
        ss << std::put_time(local_time, "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }
};