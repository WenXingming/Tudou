#pragma once

#include <unordered_map>
#include <mutex>

#include "IFileMetaStore.h"

class InMemoryFileMetaStore : public IFileMetaStore {
public:
    // Demo 实现：进程内 map。
    // 特点：重启即丢数据；优点：无外部依赖，便于跑通链路。
    bool put(const FileMetadata& meta) override;
    bool get(const std::string& fileId, FileMetadata& outMeta) override;

private:
    std::mutex mutex_;
    std::unordered_map<std::string, FileMetadata> map_;
};
