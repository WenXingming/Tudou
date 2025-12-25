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
    // FileLinkService 是“业务编排层”：
    // - 不关心 HTTP（状态码、header、路由）
    // - 只关心 upload/download 的业务流程，以及对 storage/meta/cache 的组合调用
    FileLinkService(FileSystemStorage storage,
                    std::shared_ptr<IFileMetaStore> metaStore,
                    std::shared_ptr<IFileMetaCache> metaCache);

    UploadResult upload(const std::string& originalName,
                        const std::string& contentType,
                        const std::string& fileContent);

    // 大文件上传：HTTP 层可先把 body 流式写到临时文件，再交给 service 生成 fileId 并落到 storageRoot。
    // tempPath 会在成功后被 move/copy 到 storageRoot/{fileId}，失败时尽量保持 tempPath 不变便于排查。
    UploadResult upload_from_path(const std::string& originalName,
                                  const std::string& contentType,
                                  const std::string& tempPath,
                                  int64_t fileSize);

    bool download(const std::string& fileId, DownloadResult& out);

private:
    FileSystemStorage storage_;
    std::shared_ptr<IFileMetaStore> metaStore_;
    std::shared_ptr<IFileMetaCache> metaCache_;

    // 缓存策略：meta 的 TTL（内容本身不缓存，避免内存爆炸）。
    int cacheTtlSeconds_ = 300;
};
