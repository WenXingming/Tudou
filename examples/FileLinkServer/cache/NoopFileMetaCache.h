#pragma once

#include "IFileMetaCache.h"

class NoopFileMetaCache : public IFileMetaCache {
public:
    // 默认缓存实现：永远 miss。即没有缓存效果。
    // 用途：在不接 Redis 的情况下保持代码路径一致（service 仍然走 cache-aside 流程）。
    bool put(const FileMetadata& /*meta*/, int /*ttlSeconds*/) override { return true; }
    bool get(const std::string& /*fileId*/, FileMetadata& /*outMeta*/) override { return false; }
};
