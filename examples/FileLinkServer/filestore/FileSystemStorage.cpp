#include "FileSystemStorage.h"

#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <fstream>
#include <sstream>

FileSystemStorage::FileSystemStorage(std::string rootDir) :
    rootDir_(std::move(rootDir)) {
}

bool FileSystemStorage::ensureRootExists() {
    // 简单检查 rootDir_ 是否存在，若不存在则创建之
    if (rootDir_.empty()) {
        return false;
    }

    // 使用 stat() 检查路径是否存在且为目录
    struct stat st;
    if (stat(rootDir_.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // mkdir -p 的最简实现：仅保证单级目录存在。
    // 生产环境如果传入多级目录（a/b/c），这里需要递归创建。
    // stat() 失败，尝试创建单级目录
    if (mkdir(rootDir_.c_str(), 0755) == 0) {
        return true;
    }

    // mkdir 失败，检查错误原因是否为目录已存在
    if (errno == EEXIST) {
        return true;
    }

    // 其他错误
    return false;
}

bool FileSystemStorage::save(const std::string& fileId, const std::string& content, std::string& outPath) {
    if (!ensureRootExists()) {
        return false;
    }

    // 构造完整路径：rootDir/{hash}
    std::string path = rootDir_;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path += fileId;

    // 写入文件
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
