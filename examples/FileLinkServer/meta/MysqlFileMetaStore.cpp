#include "MysqlFileMetaStore.h"

bool MysqlFileMetaStore::put(const FileMetadata& /*meta*/) {
    // TODO: 使用 MySQL C API 或 mysql-connector-c++ 实现 INSERT。
    // 建议表结构：
    // CREATE TABLE file_meta (
    //   file_id VARCHAR(64) PRIMARY KEY,
    //   original_name VARCHAR(255) NOT NULL,
    //   storage_path VARCHAR(512) NOT NULL,
    //   content_type VARCHAR(128) NOT NULL,
    //   file_size BIGINT NOT NULL,
    //   created_at_unix BIGINT NOT NULL
    // );
    return false;
}

bool MysqlFileMetaStore::get(const std::string& /*fileId*/, FileMetadata& /*outMeta*/) {
    // TODO: 使用 MySQL C API 或 mysql-connector-c++ 实现 SELECT。
    return false;
}
