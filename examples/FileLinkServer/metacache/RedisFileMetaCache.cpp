/**
 * @file RedisFileMetaCache.cpp
 * @brief 基于 hiredis 的文件元数据缓存实现
 * @details 使用 Redis 作为后端存储，实现文件元数据的缓存功能。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include "RedisFileMetaCache.h"

#if FILELINK_WITH_HIREDIS

#include <hiredis/hiredis.h>
#include <stdlib.h>
#include <spdlog/spdlog.h>

RedisFileMetaCache::RedisFileMetaCache(std::string host, int port) :
    host_(std::move(host)),
    port_(port),
    mutex_(),
    ctx_(nullptr) {

    // 构造函数不连接 Redis，采用懒连接策略
}

bool RedisFileMetaCache::put(const FileMetadata& meta, int ttlSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected()) {
        return false;
    }

    // redis 的 Hash 中存储的 key 为 filelink:file:{id}、字段为属性名
    const std::string key = make_key(meta.fileId);

    // 使用 redis 的 HSET 命令存储 Hash 结构
    std::string redisCommandStr = "HSET " + key +
        " fileId " + meta.fileId +
        " originalName " + meta.originalName +
        " storagePath " + meta.storagePath +
        " contentType " + meta.contentType +
        " fileSize " + std::to_string(meta.fileSize) +
        " createdAtUnix " + std::to_string(meta.createdAtUnix);
    redisReply* r1 = static_cast<redisReply*>(redisCommand(ctx_, redisCommandStr.c_str()));
    if (!r1) {
        spdlog::error("Redis HSET command failed for key {}", key);
        redisFree(ctx_);
        ctx_ = nullptr;
        return false;
    }
    freeReplyObject(r1);

    // 设置过期时间（如果 ttlSeconds >= 0）
    if (ttlSeconds < 0) {
        return true;
    }
    std::string redisExpireCmd = "EXPIRE " + key + " " + std::to_string(ttlSeconds);
    redisReply* r2 = static_cast<redisReply*>(redisCommand(ctx_, redisExpireCmd.c_str()));
    if (!r2) {
        spdlog::error("Redis EXPIRE command failed for key {}", key);
        redisFree(ctx_);
        ctx_ = nullptr;
        return false;
    }
    freeReplyObject(r2);

    return true;
}

bool RedisFileMetaCache::get(const std::string& fileId, FileMetadata& outMeta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected()) {
        return false;
    }

    // redis 的 Hash 中存储的 key 为 filelink:file:{id}、字段为属性名
    const std::string key = make_key(fileId);
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "HGETALL %s", key.c_str()));
    if (!r) {
        spdlog::error("Redis HGETALL command failed for key {}", key);
        redisFree(ctx_);
        ctx_ = nullptr;
        return false;
    }

    bool ok = false;
    if (r->type == REDIS_REPLY_ARRAY && (r->elements % 2 == 0)) {
        // 形如 [k1,v1,k2,v2,...]
        for (size_t i = 0; i + 1 < r->elements; i += 2) {
            const redisReply* rk = r->element[i];
            const redisReply* rv = r->element[i + 1];
            if (!rk || !rv || rk->type != REDIS_REPLY_STRING) {
                continue;
            }
            const std::string k = rk->str ? rk->str : "";
            const std::string v = (rv->type == REDIS_REPLY_STRING && rv->str) ? rv->str : "";

            if (k == "fileId") outMeta.fileId = v;
            else if (k == "originalName") outMeta.originalName = v;
            else if (k == "storagePath") outMeta.storagePath = v;
            else if (k == "contentType") outMeta.contentType = v;
            else if (k == "fileSize") outMeta.fileSize = static_cast<int64_t>(atoll(v.c_str()));
            else if (k == "createdAtUnix") outMeta.createdAtUnix = static_cast<int64_t>(atoll(v.c_str()));
        }
        ok = !outMeta.storagePath.empty();
    }

    freeReplyObject(r);
    return ok;
}

bool RedisFileMetaCache::ensure_connected() {
    // 懒连接。即第一次用时才连接 Redis。
    // 已经连接且状态良好
    if (ctx_ && ctx_->err == 0) {
        return true;
    }

    // 已经连接但状态异常：释放掉（不 return，因为后续会重新建立连接）
    if (ctx_ && ctx_->err != 0) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
    // 建立新连接
    ctx_ = redisConnect(host_.c_str(), port_);
    if (!ctx_) {
        spdlog::error("Failed to connect to Redis at {}:{}", host_, port_);
        return false;
    }
    if (ctx_->err != 0) {
        spdlog::error("Redis connection error: {}", ctx_->errstr);
        redisFree(ctx_);
        ctx_ = nullptr;
        return false;
    }

    return true;
}

std::string RedisFileMetaCache::make_key(const std::string& fileId) const {
    return std::string("filelink:file:") + fileId;
}

#else

 // 未启用 hiredis 时，提供 stub：可编译但不可用。
 // main 会在这种构建配置下回退到 NoopFileMetaCache。

bool RedisFileMetaCache::put(const FileMetadata& /*meta*/, int /*ttlSeconds*/) {
    return false;
}

bool RedisFileMetaCache::get(const std::string& /*fileId*/, FileMetadata& /*outMeta*/) {
    return false;
}

#endif
