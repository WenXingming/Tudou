#pragma once

#include <string>

class FileSystemStorage {
public:
    explicit FileSystemStorage(std::string rootDir);

    const std::string& rootDir() const { return rootDir_; }

    bool ensureRootExists();

    // 保存文件内容到磁盘：rootDir/{fileId}
    bool save(const std::string& fileId, const std::string& content, std::string& outPath);

    bool readAll(const std::string& path, std::string& outContent) const;

private:
    std::string rootDir_;
};
