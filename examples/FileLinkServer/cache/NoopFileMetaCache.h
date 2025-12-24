#pragma once

#include "IFileMetaCache.h"

class NoopFileMetaCache : public IFileMetaCache {
public:
    bool put(const FileMetadata& /*meta*/, int /*ttlSeconds*/) override { return true; }
    bool get(const std::string& /*fileId*/, FileMetadata& /*outMeta*/) override { return false; }
};
