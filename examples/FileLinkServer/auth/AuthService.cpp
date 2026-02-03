/**
 * @file AuthService.cpp
 * @brief 认证服务的实现
 * @details 该文件实现了AuthService类，提供基于用户名和密码的认证功能，并通过令牌（token）管理会话有效期。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include "AuthService.h"

#include <ctime>

#include "../utils/Uuid.h"

namespace filelink {

AuthService::AuthService(AuthConfig cfg)
    : cfg_(std::move(cfg)) {
}

bool AuthService::enabled() const {
    return cfg_.enabled_;
}

bool AuthService::check_credentials(const std::string& user, const std::string& password) const {
    return user == cfg_.user_ && password == cfg_.password_;
}

std::string AuthService::issue_token() {
    // 签发一个新的令牌（token）。
    // 该函数用于生成一个新的认证令牌（token），并将其存储在 tokenExpiry_ 映射中，关联一个过期时间。
    const std::string token = filelink::generate_hex_uuid32();
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const int64_t ttl = cfg_.tokenTtlSeconds_ > 0 ? cfg_.tokenTtlSeconds_ : 3600;

    std::lock_guard<std::mutex> lock(mu_);
    tokenExpiry_[token] = now + ttl;
    return token;
}

bool AuthService::validate_token(const std::string& token) {
    // 该函数用于验证传入的令牌（token）是否有效。
    // 因为该函数有副作用，会进行过期令牌的清理操作。所以没有命名为 is_token_valid 之类的纯查询函数。
    if (token.empty()) {
        return false;
    }
    const int64_t now = static_cast<int64_t>(::time(nullptr));

    std::lock_guard<std::mutex> lock(mu_);
    cleanup_expired_locked(now);

    const auto it = tokenExpiry_.find(token);
    return it != tokenExpiry_.end() && it->second > now;
}

void AuthService::cleanup_expired_locked(int64_t now) {
    // 每一次验证令牌时，都会调用该函数来清理过期的令牌。
    // 遍历算法显然不是最高效的，可以考虑使用更复杂的数据结构来优化性能（增加一个同步的有序数组、最小堆等进行反向索引）。
    for (auto it = tokenExpiry_.begin(); it != tokenExpiry_.end();) {
        if (it->second <= now) {
            it = tokenExpiry_.erase(it);
        }
        else {
            ++it;
        }
    }
}

} // namespace filelink
