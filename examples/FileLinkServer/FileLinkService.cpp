#include "FileLinkService.h"

#include <time.h>

#include "util/Uuid.h"
#include "util/HttpUtil.h"

FileLinkService::FileLinkService(FileSystemStorage storage,
                                 std::shared_ptr<IFileMetaStore> metaStore,
                                 std::shared_ptr<IFileMetaCache> metaCache)
    : storage_(std::move(storage)),
      metaStore_(std::move(metaStore)),
      metaCache_(std::move(metaCache)) {}

UploadResult FileLinkService::upload(const std::string& originalName,
                                     const std::string& contentType,
                                     const std::string& fileContent) {
    UploadResult result;

    const std::string fileId = filelink::generate_hex_uuid32();

    std::string storagePath;
    const bool ok = storage_.save(fileId, fileContent, storagePath);
    if (!ok) {
        return result;
    }

    FileMetadata meta;
    meta.fileId = fileId;
    meta.originalName = originalName.empty() ? "unknown" : originalName;
    meta.storagePath = storagePath;
    meta.contentType = !contentType.empty() ? contentType : filelink::guess_content_type_by_name(meta.originalName);
    meta.fileSize = static_cast<int64_t>(fileContent.size());
    meta.createdAtUnix = static_cast<int64_t>(::time(nullptr));

    if (metaStore_) {
        metaStore_->put(meta);
    }
    if (metaCache_) {
        metaCache_->put(meta, cacheTtlSeconds_);
    }

    result.fileId = fileId;
    result.urlPath = "/file/" + fileId;
    return result;
}

bool FileLinkService::download(const std::string& fileId, DownloadResult& out) {
    FileMetadata meta;

    if (metaCache_ && metaCache_->get(fileId, meta)) {
        // cache hit
    } else {
        if (!metaStore_ || !metaStore_->get(fileId, meta)) {
            return false;
        }
        if (metaCache_) {
            metaCache_->put(meta, cacheTtlSeconds_);
        }
    }

    std::string content;
    if (!storage_.readAll(meta.storagePath, content)) {
        return false;
    }

    out.meta = meta;
    out.content = std::move(content);
    return true;
}
