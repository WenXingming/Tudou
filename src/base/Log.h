/**
 * @file Log.h
 * @brief 暴露给用户使用的，只需要使用 LOG::LOG_INFO(msg) 等
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#pragma once
#include <string>

class Logger;
class LOG {
private:
    static bool info;
    static bool error;
    static bool debug;
    static bool fatal;

public:
    LOG() {}
    ~LOG() {}
    
    static void enable_info() { LOG::info = true; }
    static void enable_error() { LOG::error = true; }
    static void enable_debug() { LOG::debug = true; }
    static void enable_fatal() { LOG::fatal = true; }
    static void disable_info() { LOG::info = false; }
    static void disable_error() { LOG::error = false; }
    static void disable_debug() { LOG::debug = false; }
    static void disable_fatal() { LOG::fatal = false; }

    static void LOG_INFO(const char* fmt, ...);
    static void LOG_ERROR(const char* fmt, ...);
    static void LOG_FATAL(const char* fmt, ...);
    static void LOG_DEBUG(const char* fmt, ...);
};