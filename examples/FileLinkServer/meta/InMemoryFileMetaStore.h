#pragma once

#include <unordered_map>
#include <mutex>

#include "IFileMetaStore.h"

class InMemoryFileMetaStore : public IFileMetaStore {
public:
    bool put(const FileMetadata& meta) override;
    bool get(const std::string& fileId, FileMetadata& outMeta) override;

private:
    std::mutex mutex_;
    std::unordered_map<std::string, FileMetadata> map_;
};
