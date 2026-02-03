/**
 * @file RedisFileMetaCache.h
 * @brief 基于 hiredis 的文件元数据缓存实现
 * @details 使用 Redis 作为后端存储，实现文件元数据的缓存功能。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <mutex>

#include "IFileMetaCache.h"

 // 基于 hiredis 的元数据缓存实现

class RedisFileMetaCache : public IFileMetaCache {
public:
    RedisFileMetaCache(std::string host, int port);

    bool put(const FileMetadata& meta, int ttlSeconds) override;
    bool get(const std::string& fileId, FileMetadata& outMeta) override;

private:
    // Redis 结构：每个 fileId 一个 Hash，key 为 filelink:file:{id}。
    // 字段和值都存成字符串，跨语言/跨版本最省心。

    bool ensure_connected();
    std::string make_key(const std::string& fileId) const;

private:
    std::string host_;
    int port_;

    std::mutex mutex_;

    struct redisContext* ctx_ = nullptr;
};
