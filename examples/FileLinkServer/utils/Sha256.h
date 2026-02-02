#pragma once

#include <string>

#include <openssl/evp.h>

#include <fstream>
#include <vector>

namespace filelink {

namespace detail {

inline std::string to_hex_lower(const unsigned char* bytes, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char b = bytes[i];
        out[i * 2 + 0] = kHex[(b >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[(b) & 0x0F];
    }
    return out;
}

} // namespace detail

// Returns lowercase hex string (64 chars) for SHA-256 of input.
inline std::string sha256_hex(const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return std::string();
    }

    const EVP_MD* md = EVP_sha256();
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return std::string();
    }

    if (!data.empty()) {
        if (EVP_DigestUpdate(ctx, data.data(), data.size()) != 1) {
            EVP_MD_CTX_free(ctx);
            return std::string();
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digestLen) != 1) {
        EVP_MD_CTX_free(ctx);
        return std::string();
    }
    EVP_MD_CTX_free(ctx);

    return detail::to_hex_lower(digest, static_cast<size_t>(digestLen));
}

// Returns true on success and fills outHex with lowercase hex (64 chars).
inline bool sha256_file_hex(const std::string& path, std::string& outHex) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }

    const EVP_MD* md = EVP_sha256();
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }

    static const size_t kBufSize = 1024 * 1024; // 1MB
    std::vector<char> buf(kBufSize);
    while (ifs) {
        ifs.read(buf.data(), static_cast<std::streamsize>(kBufSize));
        const std::streamsize n = ifs.gcount();
        if (n > 0) {
            if (EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(n)) != 1) {
                EVP_MD_CTX_free(ctx);
                return false;
            }
        }
    }

    if (!ifs.eof() && ifs.fail()) {
        EVP_MD_CTX_free(ctx);
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digestLen) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    EVP_MD_CTX_free(ctx);

    outHex = detail::to_hex_lower(digest, static_cast<size_t>(digestLen));
    return true;
}

} // namespace filelink
