#include "RedisFileMetaCache.h"

#if FILELINK_WITH_HIREDIS

#include <hiredis/hiredis.h>

#include <stdlib.h>

static bool is_ok_status(const redisReply* r) {
    if (!r) return false;
    if (r->type == REDIS_REPLY_STATUS) {
        return true;
    }
    if (r->type == REDIS_REPLY_STRING) {
        return true;
    }
    return false;
}

bool RedisFileMetaCache::ensure_connected() {
    if (ctx_ && ctx_->err == 0) {
        return true;
    }

    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }

    ctx_ = redisConnect(host_.c_str(), port_);
    if (!ctx_) {
        return false;
    }
    if (ctx_->err != 0) {
        redisFree(ctx_);
        ctx_ = nullptr;
        return false;
    }

    return true;
}

std::string RedisFileMetaCache::make_key(const std::string& fileId) const {
    return std::string("filelink:file:") + fileId;
}

bool RedisFileMetaCache::put(const FileMetadata& meta, int ttlSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected()) {
        return false;
    }

    const std::string key = make_key(meta.fileId);

    // hiredis 参数格式化：%s / %lld
    redisReply* r1 = static_cast<redisReply*>(redisCommand(
        ctx_,
        "HSET %s fileId %s originalName %s storagePath %s contentType %s fileSize %lld createdAtUnix %lld",
        key.c_str(),
        meta.fileId.c_str(),
        meta.originalName.c_str(),
        meta.storagePath.c_str(),
        meta.contentType.c_str(),
        static_cast<long long>(meta.fileSize),
        static_cast<long long>(meta.createdAtUnix)));

    if (!r1) {
        redisFree(ctx_);
        ctx_ = nullptr;
        return false;
    }
    freeReplyObject(r1);

    if (ttlSeconds > 0) {
        redisReply* r2 = static_cast<redisReply*>(redisCommand(ctx_, "EXPIRE %s %d", key.c_str(), ttlSeconds));
        if (!r2) {
            redisFree(ctx_);
            ctx_ = nullptr;
            return false;
        }
        freeReplyObject(r2);
    }

    return true;
}

bool RedisFileMetaCache::get(const std::string& fileId, FileMetadata& outMeta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected()) {
        return false;
    }

    const std::string key = make_key(fileId);
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "HGETALL %s", key.c_str()));
    if (!r) {
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

#else

bool RedisFileMetaCache::put(const FileMetadata& /*meta*/, int /*ttlSeconds*/) {
    return false;
}

bool RedisFileMetaCache::get(const std::string& /*fileId*/, FileMetadata& /*outMeta*/) {
    return false;
}

#endif
