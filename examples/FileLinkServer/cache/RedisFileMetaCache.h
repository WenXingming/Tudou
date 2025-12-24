#pragma once

#include "IFileMetaCache.h"

// 说明：
//  - 这里先放一个“可替换实现”的骨架，避免强依赖 hiredis 导致你当前工程无法编译。
//  - 你部署好 Redis 并希望我把它实现成可用版本时，我可以继续补齐 .cpp + CMake link。

class RedisFileMetaCache : public IFileMetaCache {
public:
    RedisFileMetaCache(std::string host, int port)
        : host_(std::move(host)), port_(port) {}

    bool put(const FileMetadata& meta, int ttlSeconds) override;
    bool get(const std::string& fileId, FileMetadata& outMeta) override;

private:
    std::string host_;
    int port_;
};
