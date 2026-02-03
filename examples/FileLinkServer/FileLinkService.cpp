/**
 * @file FileLinkService.h
 * @brief 上传下载服务的实现
 * @details 负责 upload/download 业务流程的编排，调用 filestore/metastore/metacache 完成具体存储与缓存操作。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#include "FileLinkService.h"

#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <errno.h>

#include "utils/Uuid.h"
#include "utils/HttpUtil.h"
#include "utils/Sha256.h"

namespace {

static const char* kUnknownFileName = "unknown";

bool get_file_size(const std::string& path, int64_t& outSize) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    outSize = static_cast<int64_t>(st.st_size);
    return true;
}

bool copy_file(const std::string& srcPath, const std::string& dstPath) {
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

bool path_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// 输入目录路径，确保该目录存在（若不存在则创建），仅支持单级目录创建。
bool ensure_dir_exists_single_level(const std::string& dirPath) {
    if (dirPath.empty()) {
        return false;
    }

    struct stat st;
    if (::stat(dirPath.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (::mkdir(dirPath.c_str(), 0755) == 0) {
        return true;
    }

    if (errno == EEXIST) {
        return true;
    }

    return false;
}

std::string join_path2(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

// Write fileContent into a blob file addressed by sha256Hex, without overwriting existing.
// Uses a tmp file + link() for atomic create semantics.
// 输入文件内容，写入到以 sha256Hex 命名的 blob 文件中，若已存在则不覆盖。
bool ensure_blob_from_content(const std::string& blobDir,
    const std::string& sha256Hex,
    const std::string& fileContent,
    std::string& outBlobPath) {
    outBlobPath = join_path2(blobDir, sha256Hex);
    if (path_exists(outBlobPath)) {
        return true;
    }

    const std::string tmpName = "." + sha256Hex + ".tmp." + filelink::generate_hex_uuid32();
    const std::string tmpPath = join_path2(blobDir, tmpName);

    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        if (!ofs) {
            return false;
        }
        if (!fileContent.empty()) {
            ofs.write(fileContent.data(), static_cast<std::streamsize>(fileContent.size()));
        }
        ofs.close();
        if (!ofs) {
            (void)::unlink(tmpPath.c_str());
            return false;
        }
    }

    if (::link(tmpPath.c_str(), outBlobPath.c_str()) == 0) {
        (void)::unlink(tmpPath.c_str());
        return true;
    }

    if (errno == EEXIST) {
        // Another request won the race.
        (void)::unlink(tmpPath.c_str());
        return true;
    }

    (void)::unlink(tmpPath.c_str());
    return false;
}

// Move/copy temp file into blob addressed by sha256Hex, without overwriting existing.
// Uses a tmp file in blobDir + link() for atomic create semantics.
bool ensure_blob_from_tempfile(const std::string& blobDir,
    const std::string& sha256Hex,
    const std::string& tempPath,
    std::string& outBlobPath) {
    outBlobPath = join_path2(blobDir, sha256Hex);
    if (path_exists(outBlobPath)) {
        (void)::unlink(tempPath.c_str());
        return true;
    }

    const std::string tmpName = "." + sha256Hex + ".tmp." + filelink::generate_hex_uuid32();
    const std::string tmpPath = join_path2(blobDir, tmpName);

    bool moved = (::rename(tempPath.c_str(), tmpPath.c_str()) == 0);
    if (!moved) {
        if (!copy_file(tempPath, tmpPath)) {
            return false;
        }
        (void)::unlink(tempPath.c_str());
    }

    if (::link(tmpPath.c_str(), outBlobPath.c_str()) == 0) {
        (void)::unlink(tmpPath.c_str());
        return true;
    }

    if (errno == EEXIST) {
        (void)::unlink(tmpPath.c_str());
        return true;
    }

    // Keep tmp for debugging? In demo, cleanup.
    (void)::unlink(tmpPath.c_str());
    return false;
}

FileMetadata build_meta(const std::string& fileId,
    const std::string& originalName,
    const std::string& contentType,
    const std::string& storagePath,
    int64_t fileSize) {
    FileMetadata meta;
    meta.fileId = fileId;
    meta.originalName = originalName.empty() ? kUnknownFileName : originalName;
    meta.storagePath = storagePath;
    meta.contentType = !contentType.empty() ? contentType : filelink::guess_content_type_by_name(meta.originalName);
    meta.fileSize = fileSize > 0 ? fileSize : 0;
    meta.createdAtUnix = static_cast<int64_t>(::time(nullptr));
    return meta;
}

// 输入元数据，持久化到 metaStore 和 metaCache。
void persist_meta(const std::shared_ptr<IFileMetaStore>& metaStore,
    const std::shared_ptr<IFileMetaCache>& metaCache,
    const FileMetadata& meta,
    int cacheTtlSeconds) {

    if (metaStore) {
        metaStore->put(meta);
    }
    if (metaCache) {
        metaCache->put(meta, cacheTtlSeconds);
    }
}

UploadResult make_upload_result(const std::string& fileId) {
    UploadResult r;
    r.fileId = fileId;
    r.urlPath = "/file/" + fileId;
    return r;
}

} // namespace

FileLinkService::FileLinkService(FileSystemStorage storage,
    std::shared_ptr<IFileMetaStore> metaStore,
    std::shared_ptr<IFileMetaCache> metaCache)
    : storage_(std::move(storage)),
    metaStore_(std::move(metaStore)),
    metaCache_(std::move(metaCache)) {
}

UploadResult FileLinkService::upload(const std::string& originalName, const std::string& contentType, const std::string& fileContent) {
    // 软去重：
    // - 对外仍然使用随机 fileId（每次上传一个新链接）
    // - 对内文件内容落到 blobs/{sha256}（相同内容只存一份）
    const std::string fileId = filelink::generate_hex_uuid32();

    // 确保 storage root 及 blobs 子目录存在。
    if (!storage_.ensureRootExists()) {
        return UploadResult();
    }
    const std::string blobDir = join_path2(storage_.rootDir(), "blobs");
    if (!ensure_dir_exists_single_level(blobDir)) {
        return UploadResult();
    }

    // 把文件内容写入 blobs/{sha256}。
    const std::string sha256Hex = filelink::sha256_hex(fileContent);
    std::string storagePath;
    if (!ensure_blob_from_content(blobDir, sha256Hex, fileContent, storagePath)) {
        return UploadResult();
    }

    // 构建元数据，并推送到 store（MySQL）和 cache（Redis）。
    const FileMetadata meta = build_meta(
        fileId,
        originalName,
        contentType,
        storagePath,
        static_cast<int64_t>(fileContent.size()));
    persist_meta(metaStore_, metaCache_, meta, cacheTtlSeconds_);
    return make_upload_result(fileId);
}

UploadResult FileLinkService::upload_from_path(const std::string& originalName,
    const std::string& contentType,
    const std::string& tempPath,
    int64_t fileSize) {
    if (tempPath.empty()) {
        return UploadResult();
    }

    const std::string fileId = filelink::generate_hex_uuid32();

    if (!storage_.ensureRootExists()) {
        return UploadResult();
    }
    const std::string blobDir = join_path2(storage_.rootDir(), "blobs");
    if (!ensure_dir_exists_single_level(blobDir)) {
        return UploadResult();
    }

    std::string sha256Hex;
    if (!filelink::sha256_file_hex(tempPath, sha256Hex)) {
        return UploadResult();
    }

    std::string blobPath;
    if (!ensure_blob_from_tempfile(blobDir, sha256Hex, tempPath, blobPath)) {
        return UploadResult();
    }

    if (fileSize <= 0) {
        (void)get_file_size(blobPath, fileSize);
    }

    const FileMetadata meta = build_meta(fileId, originalName, contentType, blobPath, fileSize);
    persist_meta(metaStore_, metaCache_, meta, cacheTtlSeconds_);
    return make_upload_result(fileId);
}

bool FileLinkService::download(const std::string& fileId, DownloadResult& out) {
    FileMetadata meta;

    // cache-aside：优先查 cache，miss 时回源 store，再把结果写回 cache。
    if (metaCache_ && metaCache_->get(fileId, meta)) {
        // cache hit
        // 不需要额外操作
    }
    else {
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
