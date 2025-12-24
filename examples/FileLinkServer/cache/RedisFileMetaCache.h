#pragma once

#include <mutex>

#include "IFileMetaCache.h"

// 基于 hiredis 的元数据缓存实现

class RedisFileMetaCache : public IFileMetaCache {
public:
    RedisFileMetaCache(std::string host, int port)
        : host_(std::move(host)), port_(port) {}

    bool put(const FileMetadata& meta, int ttlSeconds) override;
    bool get(const std::string& fileId, FileMetadata& outMeta) override;

private:
    bool ensure_connected();
    std::string make_key(const std::string& fileId) const;

    std::string host_;
    int port_;

    std::mutex mutex_;

    struct redisContext* ctx_ = nullptr;
};
