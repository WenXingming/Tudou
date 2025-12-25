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
    // Redis 结构：每个 fileId 一个 Hash，key 为 filelink:file:{id}。
    // 字段和值都存成字符串，跨语言/跨版本最省心。

    bool ensure_connected(); // 作用：懒连接。即第一次用时才连接 Redis。
    std::string make_key(const std::string& fileId) const;

    std::string host_;
    int port_;

    std::mutex mutex_;

    struct redisContext* ctx_ = nullptr;
};
