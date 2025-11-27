/**
 * @file Logger.cpp
 * @brief 日志类
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#include "Logger.h"
#include "Timestamp.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

Logger& Logger::get_instance() {
    static Logger logger;
    return logger;
}

void Logger::set_log_level(LogLevel level) {
    this->logLevel = level;
}

void Logger::log(std::string msg) {
    switch (this->logLevel) {
    case LogLevel::INFO:
        std::cout << "[INFO]: ";
        break;
    case LogLevel::ERROR:
        std::cout << "[ERROR]: ";
        break;
    case LogLevel::FATAL:
        std::cout << "[FATAL]: ";
        break;
    case LogLevel::DEBUG:
        std::cout << "[DEBUG]: ";
        break;
    default:
        break;
    }

    // 打印时间和 msg
    // 获取时间
    auto now = std::chrono::system_clock::now();
    std::cout << Timestamp(now).timestamp_to_string() << ". message: " << msg << std::endl;
}
