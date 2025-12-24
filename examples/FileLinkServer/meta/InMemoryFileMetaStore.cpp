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
