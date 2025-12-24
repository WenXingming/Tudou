#pragma once

#include <memory>
#include <string>

#include "storage/FileSystemStorage.h"
#include "meta/IFileMetaStore.h"
#include "cache/IFileMetaCache.h"

struct UploadResult {
    std::string fileId;
    std::string urlPath; // /file/{id}
};

struct DownloadResult {
    FileMetadata meta;
    std::string content;
};

class FileLinkService {
public:
    FileLinkService(FileSystemStorage storage,
                    std::shared_ptr<IFileMetaStore> metaStore,
                    std::shared_ptr<IFileMetaCache> metaCache);

    UploadResult upload(const std::string& originalName,
                        const std::string& contentType,
                        const std::string& fileContent);

    bool download(const std::string& fileId, DownloadResult& out);

private:
    FileSystemStorage storage_;
    std::shared_ptr<IFileMetaStore> metaStore_;
    std::shared_ptr<IFileMetaCache> metaCache_;

    int cacheTtlSeconds_ = 300;
};
