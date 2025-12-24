#pragma once

#include <string>

#include "FileMetadata.h"

class IFileMetaStore {
public:
    virtual ~IFileMetaStore() = default;

    virtual bool put(const FileMetadata& meta) = 0;
    virtual bool get(const std::string& fileId, FileMetadata& outMeta) = 0;
};
