/**
 * @file InMemoryFileMetaStore.h
 * @brief 文件元数据存储接口的内存实现
 * @details 提供一个基于进程内存的文件元数据存储实现。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include "InMemoryFileMetaStore.h"

bool InMemoryFileMetaStore::put(const FileMetadata& meta) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[meta.fileId] = meta;
    return true;
}

bool InMemoryFileMetaStore::get(const std::string& fileId, FileMetadata& outMeta) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(fileId);
    if (it == map_.end()) {
        return false;
    }
    outMeta = it->second;
    return true;
}
