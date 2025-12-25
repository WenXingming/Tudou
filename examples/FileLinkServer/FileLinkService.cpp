#include "FileLinkService.h"

#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>

#include "util/Uuid.h"
#include "util/HttpUtil.h"

static bool get_file_size(const std::string& path, int64_t& outSize) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    outSize = static_cast<int64_t>(st.st_size);
    return true;
}

static bool copy_file(const std::string& srcPath, const std::string& dstPath) {
    std::ifstream src(srcPath, std::ios::binary);
    if (!src) return false;
    std::ofstream dst(dstPath, std::ios::binary | std::ios::trunc);
    if (!dst) return false;

    static const size_t kBufSize = 1024 * 1024;
    char buf[kBufSize];
    while (src) {
        src.read(buf, static_cast<std::streamsize>(kBufSize));
        std::streamsize n = src.gcount();
        if (n > 0) {
            dst.write(buf, n);
            if (!dst) return false;
        }
    }
    return true;
}

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

    // 业务主键：这里用随机 32hex 字符串，作为
    // - 对外暴露的下载 id（/file/{id}）
    // - 对内落盘文件名（rootDir/{id}）
    const std::string fileId = filelink::generate_hex_uuid32();

    // 第一阶段：落盘（成功后才写 meta）。
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

    // 第二阶段：写入元数据（store）+ 热数据（cache）。
    // demo 里不强制要求 store/cache 成功，避免因为基础设施不可用阻断上传。
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

UploadResult FileLinkService::upload_from_path(const std::string& originalName,
                                               const std::string& contentType,
                                               const std::string& tempPath,
                                               int64_t fileSize) {
    UploadResult result;
    if (tempPath.empty()) {
        return result;
    }

    const std::string fileId = filelink::generate_hex_uuid32();

    // 目标路径：storageRoot/{fileId}
    std::string finalPath = storage_.rootDir();
    if (!finalPath.empty() && finalPath.back() != '/') {
        finalPath.push_back('/');
    }
    finalPath += fileId;

    // 确保目录存在
    if (!storage_.ensureRootExists()) {
        return result;
    }

    // 优先 rename（同一文件系统几乎 O(1)）；失败则 copy+unlink。
    bool moved = (::rename(tempPath.c_str(), finalPath.c_str()) == 0);
    if (!moved) {
        if (!copy_file(tempPath, finalPath)) {
            return result;
        }
        (void)::unlink(tempPath.c_str());
    }

    if (fileSize <= 0) {
        (void)get_file_size(finalPath, fileSize);
    }

    FileMetadata meta;
    meta.fileId = fileId;
    meta.originalName = originalName.empty() ? "unknown" : originalName;
    meta.storagePath = finalPath;
    meta.contentType = !contentType.empty() ? contentType : filelink::guess_content_type_by_name(meta.originalName);
    meta.fileSize = fileSize > 0 ? fileSize : 0;
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

    // cache-aside：优先查 cache，miss 时回源 store，再把结果写回 cache。
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

    // meta 里存的是 storagePath，因此读取文件内容只依赖存储层。
    std::string content;
    if (!storage_.readAll(meta.storagePath, content)) {
        return false;
    }

    out.meta = meta;
    out.content = std::move(content);
    return true;
}
