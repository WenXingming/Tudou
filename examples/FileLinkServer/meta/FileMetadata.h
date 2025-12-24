#pragma once

#include <string>
#include <stdint.h>

struct FileMetadata {
    std::string fileId;
    std::string originalName;
    std::string storagePath;
    std::string contentType;
    int64_t fileSize = 0;
    int64_t createdAtUnix = 0;
};
