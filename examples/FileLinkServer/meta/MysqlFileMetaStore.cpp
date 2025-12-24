#include "MysqlFileMetaStore.h"

#if FILELINK_WITH_MYSQLCPPCONN

#include <sstream>

#include <mysql_driver.h>
#include <mysql_connection.h>

#include <cppconn/statement.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/exception.h>

static std::string build_mysql_url(const std::string& host, int port) {
    std::ostringstream oss;
    oss << "tcp://" << host << ":" << port;
    return oss.str();
}

MysqlFileMetaStore::~MysqlFileMetaStore() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_) {
        delete conn_;
        conn_ = nullptr;
    }
}

bool MysqlFileMetaStore::ensure_connected() {
    if (conn_ != nullptr) {
        return true;
    }

    try {
        sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
        conn_ = driver->connect(build_mysql_url(host_, port_), user_, password_);
        if (!conn_) {
            return false;
        }
        conn_->setSchema(database_);
        return true;
    } catch (const sql::SQLException&) {
        if (conn_) {
            delete conn_;
            conn_ = nullptr;
        }
        schemaReady_ = false;
        return false;
    }
}

bool MysqlFileMetaStore::ensure_schema() {
    if (schemaReady_) {
        return true;
    }
    if (!ensure_connected()) {
        return false;
    }

    try {
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        // 用 BIGINT 存 unix time 方便跨语言
        stmt->execute(
            "CREATE TABLE IF NOT EXISTS file_meta ("
            "  file_id VARCHAR(64) PRIMARY KEY,"
            "  original_name VARCHAR(255) NOT NULL,"
            "  storage_path VARCHAR(512) NOT NULL,"
            "  content_type VARCHAR(128) NOT NULL,"
            "  file_size BIGINT NOT NULL,"
            "  created_at_unix BIGINT NOT NULL"
            ");");
        schemaReady_ = true;
        return true;
    } catch (const sql::SQLException&) {
        return false;
    }
}

bool MysqlFileMetaStore::put(const FileMetadata& meta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_schema()) {
        return false;
    }

    try {
        // upsert：存在则更新
        std::unique_ptr<sql::PreparedStatement> ps(conn_->prepareStatement(
            "INSERT INTO file_meta(file_id, original_name, storage_path, content_type, file_size, created_at_unix)"
            " VALUES(?,?,?,?,?,?)"
            " ON DUPLICATE KEY UPDATE"
            " original_name=VALUES(original_name),"
            " storage_path=VALUES(storage_path),"
            " content_type=VALUES(content_type),"
            " file_size=VALUES(file_size),"
            " created_at_unix=VALUES(created_at_unix)"));

        ps->setString(1, meta.fileId);
        ps->setString(2, meta.originalName);
        ps->setString(3, meta.storagePath);
        ps->setString(4, meta.contentType);
        ps->setInt64(5, meta.fileSize);
        ps->setInt64(6, meta.createdAtUnix);

        ps->execute();
        return true;
    } catch (const sql::SQLException&) {
        // 连接可能断了：下次重连
        if (conn_) {
            delete conn_;
            conn_ = nullptr;
        }
        schemaReady_ = false;
        return false;
    }
}

bool MysqlFileMetaStore::get(const std::string& fileId, FileMetadata& outMeta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_schema()) {
        return false;
    }

    try {
        std::unique_ptr<sql::PreparedStatement> ps(conn_->prepareStatement(
            "SELECT file_id, original_name, storage_path, content_type, file_size, created_at_unix"
            " FROM file_meta WHERE file_id=? LIMIT 1"));
        ps->setString(1, fileId);
        std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
        if (!rs || !rs->next()) {
            return false;
        }

        outMeta.fileId = rs->getString("file_id");
        outMeta.originalName = rs->getString("original_name");
        outMeta.storagePath = rs->getString("storage_path");
        outMeta.contentType = rs->getString("content_type");
        outMeta.fileSize = rs->getInt64("file_size");
        outMeta.createdAtUnix = rs->getInt64("created_at_unix");
        return true;
    } catch (const sql::SQLException&) {
        if (conn_) {
            delete conn_;
            conn_ = nullptr;
        }
        schemaReady_ = false;
        return false;
    }
}

#else

MysqlFileMetaStore::~MysqlFileMetaStore() {
}

bool MysqlFileMetaStore::put(const FileMetadata& /*meta*/) {
    return false;
}

bool MysqlFileMetaStore::get(const std::string& /*fileId*/, FileMetadata& /*outMeta*/) {
    return false;
}

#endif
