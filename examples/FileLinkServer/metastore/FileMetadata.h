/**
 * @file FileMetadata.h
 * @brief 文件元数据结构定义
 * @details 定义了文件元数据的结构体，用于描述文件的基本属性。
 * @author wenxingming
 * @date 2025-12-17
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <string>
#include <stdint.h>


struct FileMetadata {
    //文件元数据结构体
    std::string fileId;
    std::string originalName;
    std::string storagePath;
    std::string contentType;
    int64_t fileSize = 0;
    int64_t createdAtUnix = 0;
};
