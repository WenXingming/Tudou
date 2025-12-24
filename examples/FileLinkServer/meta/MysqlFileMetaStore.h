#pragma once

#include "IFileMetaStore.h"

// 说明：
//  - 这里先放一个“可替换实现”的骨架，避免强依赖 mysql client 导致你当前工程无法编译。
//  - 你安装好 libmysqlclient-dev 并希望我把它实现成可用版本时，我可以继续补齐 .cpp + CMake link。

class MysqlFileMetaStore : public IFileMetaStore {
public:
    MysqlFileMetaStore(std::string host,
                       int port,
                       std::string user,
                       std::string password,
                       std::string database)
        : host_(std::move(host)),
          port_(port),
          user_(std::move(user)),
          password_(std::move(password)),
          database_(std::move(database)) {}

    bool put(const FileMetadata& meta) override;
    bool get(const std::string& fileId, FileMetadata& outMeta) override;

private:
    std::string host_;
    int port_;
    std::string user_;
    std::string password_;
    std::string database_;
};
