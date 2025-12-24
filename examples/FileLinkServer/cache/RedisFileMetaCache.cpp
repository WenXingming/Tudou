#include "RedisFileMetaCache.h"

bool RedisFileMetaCache::put(const FileMetadata& /*meta*/, int /*ttlSeconds*/) {
    // TODO: 使用 hiredis 实现：
    //  - HSET file:{id} originalName storagePath contentType fileSize createdAtUnix
    //  - EXPIRE file:{id} ttlSeconds
    return false;
}

bool RedisFileMetaCache::get(const std::string& /*fileId*/, FileMetadata& /*outMeta*/) {
    // TODO: 使用 hiredis 实现：HGETALL 并回填 outMeta
    return false;
}
