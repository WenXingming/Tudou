#pragma once

#include <string>

#include "../meta/FileMetadata.h"

class IFileMetaCache {
public:
    virtual ~IFileMetaCache() = default;

    virtual bool put(const FileMetadata& meta, int ttlSeconds) = 0;
    virtual bool get(const std::string& fileId, FileMetadata& outMeta) = 0;
};
