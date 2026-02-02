#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace filelink {

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

    std::mutex mu_;
    std::unordered_map<std::string, int64_t> tokenExpiry_; // token -> expire unix
};

} // namespace filelink
