/**
 * @file IFileMetaStore.h
 * @brief 文件元数据存储接口（基类）
 * @details 定义了文件元数据存储的接口，供不同存储实现继承和实现。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <string>

#include "FileMetadata.h"

class IFileMetaStore {
public:
    virtual ~IFileMetaStore() = default;

    // 元数据 store：负责“fileId -> FileMetadata”的持久化。
    // 注意：HttpServer 通常是多线程的，因此实现需要自行保证并发安全。
    virtual bool put(const FileMetadata& meta) = 0;
    virtual bool get(const std::string& fileId, FileMetadata& outMeta) = 0;
};
