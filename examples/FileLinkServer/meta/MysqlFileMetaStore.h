#pragma once

#include <mutex>

#include "IFileMetaStore.h"

// 基于 mysql-connector-c++ 的元数据存储实现

namespace sql {
class Connection;
}

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

        ~MysqlFileMetaStore();

    bool put(const FileMetadata& meta) override;
    bool get(const std::string& fileId, FileMetadata& outMeta) override;

private:
    bool ensure_connected();
    bool ensure_schema();

    std::string host_;
    int port_;
    std::string user_;
    std::string password_;
    std::string database_;

    std::mutex mutex_;
    bool schemaReady_ = false;

    sql::Connection* conn_ = nullptr;
};
