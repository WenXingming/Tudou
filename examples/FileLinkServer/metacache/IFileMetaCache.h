/**
 * @file IFileMetaCache.h
 * @brief 文件元数据缓存接口（基类）
 * @details 定义了文件元数据缓存的接口，供不同缓存实现继承和实现。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <string>

#include "../metastore/FileMetadata.h"

class IFileMetaCache {
public:
    virtual ~IFileMetaCache() = default;

    // 元数据 cache：用于加速下载时的 meta 查询（cache-aside）。约定：cache 里只放 FileMetadata（不缓存文件内容）。
    // put 函数的作用是写入缓存，ttlSeconds 指定该条目的生存时间（秒）。
    // get 函数的作用是查询缓存，命中时返回 true 并填充 outMeta，未命中时返回 false。
    // 注意：HttpServer 通常是多线程的，因此实现需要自行保证并发安全。
    virtual bool put(const FileMetadata& meta, int ttlSeconds) = 0;
    virtual bool get(const std::string& fileId, FileMetadata& outMeta) = 0;
};
