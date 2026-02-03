/**
 * @file AuthService.h
 * @brief 认证服务的类
 * @details 该类实现了一个简单的认证服务，支持基于用户名和密码的认证，并通过令牌（token）管理会话有效期。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace filelink {

// Data Class for Auth Configuration
struct AuthConfig {
    bool enabled_ = false;
    std::string user_;
    std::string password_;
    int tokenTtlSeconds_ = 3600;

    AuthConfig() = default;
    AuthConfig(bool enabled, std::string user, std::string password, int tokenTtlSeconds) :
        enabled_(enabled),
        user_(std::move(user)),
        password_(std::move(password)),
        tokenTtlSeconds_(tokenTtlSeconds) {
    }
};

class AuthService {
public:
    explicit AuthService(AuthConfig cfg);

    bool enabled() const;

    bool check_credentials(const std::string& user, const std::string& password) const;

    std::string issue_token();
    bool validate_token(const std::string& token);

private:
    void cleanup_expired_locked(int64_t now);

private:
    AuthConfig cfg_;

    std::unordered_map<std::string, int64_t> tokenExpiry_; // token -> expire unix time
    std::mutex mu_;
};

} // namespace filelink
