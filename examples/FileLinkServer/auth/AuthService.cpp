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
    const std::string token = filelink::generate_hex_uuid32();
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const int64_t ttl = cfg_.tokenTtlSeconds_ > 0 ? cfg_.tokenTtlSeconds_ : 3600;

    std::lock_guard<std::mutex> lock(mu_);
    tokenExpiry_[token] = now + ttl;
    return token;
}

void AuthService::cleanup_expired_locked(int64_t now) {
    for (auto it = tokenExpiry_.begin(); it != tokenExpiry_.end();) {
        if (it->second <= now) {
            it = tokenExpiry_.erase(it);
        }
        else {
            ++it;
        }
    }
}

bool AuthService::validate_token(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    const int64_t now = static_cast<int64_t>(::time(nullptr));

    std::lock_guard<std::mutex> lock(mu_);
    cleanup_expired_locked(now);

    const auto it = tokenExpiry_.find(token);
    return it != tokenExpiry_.end() && it->second > now;
}

} // namespace filelink
