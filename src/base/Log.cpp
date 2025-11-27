/**
 * @file Log.cpp
 * @brief 暴露给用户使用的，只需要使用 LOG::LOG_INFO 等
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#include "Log.h"
#include "Logger.h"
#include <sstream>
#include <cstdarg> // For va_list, va_start, va_end
#include <cstdio>  // For vsnprintf
#include <memory>  // For std::unique_ptr
#include <iostream>

bool LOG::info = true;
bool LOG::error = true;
bool LOG::debug = true;
bool LOG::fatal = true;

void LOG::LOG_INFO(const char* fmt, ...) {
    if (!info) { return; }

    va_list args; // 声明一个 va_list 类型的变量, 用于访问可变参数列表
    va_start(args, fmt); // 初始化可变参数列表, 将 'args' 指向可变参数列表的第一个参数
    char buffer[1024]{};
    vsnprintf(buffer, 1024, fmt, args); // vsnprintf 专门用于处理 va_list，它可以将格式化后的内容
    va_end(args); // 使用 va_end 宏清理 va_list 变量。

    Logger& logger = Logger::get_instance();
    logger.set_log_level(LogLevel::INFO);
    logger.log(std::string(buffer));
}

void LOG::LOG_ERROR(const char* fmt, ...) {
    if (!error) { return; }

    va_list args;
    va_start(args, fmt);
    char buffer[1024]{};
    vsnprintf(buffer, 1024, fmt, args);
    va_end(args);

    Logger& logger = Logger::get_instance();
    logger.set_log_level(LogLevel::ERROR);
    logger.log(std::string(buffer));
}

void LOG::LOG_FATAL(const char* fmt, ...) {
    if (!fatal) { return; }

    va_list args;
    va_start(args, fmt);
    char buffer[1024]{};
    vsnprintf(buffer, 1024, fmt, args);
    va_end(args);

    Logger& logger = Logger::get_instance();
    logger.set_log_level(LogLevel::FATAL);
    logger.log(std::string(buffer));
    exit(-1); // fatal 退出程序
}

void LOG::LOG_DEBUG(const char* fmt, ...) {
    if (!debug) { return; }

    va_list args;
    va_start(args, fmt);
    char buffer[1024]{};
    vsnprintf(buffer, 1024, fmt, args);
    va_end(args);

    Logger& logger = Logger::get_instance();
    logger.set_log_level(LogLevel::DEBUG);
    logger.log(std::string(buffer));
}
