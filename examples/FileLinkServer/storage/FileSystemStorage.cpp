#include "FileSystemStorage.h"

#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <fstream>
#include <sstream>

FileSystemStorage::FileSystemStorage(std::string rootDir)
    : rootDir_(std::move(rootDir)) {}

bool FileSystemStorage::ensureRootExists() {
    if (rootDir_.empty()) {
        return false;
    }

    // mkdir -p 的最简实现：仅保证单级目录存在。
    // 生产环境如果传入多级目录（a/b/c），这里需要递归创建。
    struct stat st;
    if (stat(rootDir_.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (mkdir(rootDir_.c_str(), 0755) == 0) {
        return true;
    }

    if (errno == EEXIST) {
        return true;
    }

    return false;
}

bool FileSystemStorage::save(const std::string& fileId, const std::string& content, std::string& outPath) {
    if (!ensureRootExists()) {
        return false;
    }

    std::string path = rootDir_;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path += fileId;

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return false;
    }

    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.close();

    outPath = path;
    return true;
}

bool FileSystemStorage::readAll(const std::string& path, std::string& outContent) const {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();
    outContent = oss.str();
    return true;
}
