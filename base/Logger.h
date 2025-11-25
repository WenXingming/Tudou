/**
 * @file Logger.h
 * @brief 日志类
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#pragma once
#include <string>
#include "NonCopyable.h"

enum LogLevel {
    INFO,   // 普通流程信息
    ERROR,  // 错误信息，程序还能执行
    FATAL,  // core 信息，毁灭性打击
    DEBUG   // 开发过程中调试信息
};

class Logger : NonCopyable {
public:
    explicit Logger() : logLevel(LogLevel::INFO) {}
    ~Logger() {}

    static Logger& get_instance();

    void set_log_level(LogLevel level);

    void log(std::string msg);

private:
    int logLevel;

};