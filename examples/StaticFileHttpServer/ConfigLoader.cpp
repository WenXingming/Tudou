/**
 * @file ConfigLoader.cpp
 * @brief StaticFileHttpServer 配置加载实现（基于 CLI11 声明式绑定）
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "ConfigLoader.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <sys/stat.h>
#include <CLI/CLI.hpp>

namespace {

bool file_exists(const std::string& path) {
    std::ifstream f(path.c_str());
    return f.good();
}

std::string normalize_server_root(std::string root) {
    if (!root.empty() && root.back() != '/') {
        root.push_back('/');
    }
    return root;
}

bool ensure_dir_recursive(const std::string& dir) {
    if (dir.empty()) {
        return true;
    }
    std::string current;
    size_t pos = 0;
    if (dir[0] == '/') {
        current = "/";
        pos = 1;
    }
    while (pos < dir.size()) {
        const size_t next = dir.find('/', pos);
        const std::string part = (next == std::string::npos)
            ? dir.substr(pos)
            : dir.substr(pos, next - pos);
        if (!part.empty()) {
            if (current.size() > 1 && current.back() != '/') {
                current.push_back('/');
            }
            current += part;
            if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
}

} // namespace

bool load_static_server_config(int argc, char* argv[],
    StaticFileServerConfig& out,
    std::string& outError) {
    outError.clear();

    // 1. 两阶段参数解析：扫描命令行仅寻找 --root / -r 参数以定位配置文件
    std::string serverRoot;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && (std::strcmp(argv[i], "-r") == 0 || std::strcmp(argv[i], "--root") == 0)) {
            if (i + 1 < argc && argv[i + 1] != nullptr) {
                serverRoot = argv[i + 1];
            }
            break;
        }
    }

    // 2. 如果命令行未指定 root，按优先级在默认路径中搜索包含 conf/server.conf 的目录
    if (serverRoot.empty()) {
        const std::vector<std::string> searchRoots = {
            "/etc/static-file-http-server/",
            "./static-file-http-server/",
            "./",
            "/home/wxm/Tudou/configs/static-file-http-server/",
        };
        for (const auto& root : searchRoots) {
            std::string checkRoot = normalize_server_root(root);
            if (file_exists(checkRoot + "conf/server.conf")) {
                serverRoot = checkRoot;
                break;
            }
        }
        if (serverRoot.empty()) {
            outError = "No configuration found in default locations. Please specify server root with -r <serverRoot>.";
            return false;
        }
    }

    serverRoot = normalize_server_root(serverRoot);
    const std::string configPath = serverRoot + "conf/server.conf";

    // 3. 构建 CLI11 应用进行全量参数绑定解析（优先命令行，次之 INI 配置文件）
    CLI::App app{"Tudou StaticFileHttpServer - A high performance zero-copy static file server"};
    
    // 设置允许从 INI 格式加载的字段
    app.add_option("-r,--root", serverRoot, "Server root directory containing assets, conf, etc.");
    app.add_option("--ip", out.ip, "Bind listen IP")->configurable();
    app.add_option("--port", out.port, "Bind listen port")->configurable();
    app.add_option("--threadNum", out.threadNum, "Worker reactor thread count")->configurable();
    
    // SSL / kTLS 参数，并声明其为可配置
    app.add_option("--enableSsl", out.enableSsl, "Enable SSL/HTTPS")->configurable();
    app.add_option("--sslCert", out.sslCertPath, "Path to SSL certificate PEM file")->configurable();
    app.add_option("--sslKey", out.sslKeyPath, "Path to SSL private key PEM file")->configurable();
    app.add_option("--enableKtls", out.enableKtls, "Enable Kernel TLS (kTLS) zero-copy offloading")->configurable();

    // 指定 INI 配置文件默认值
    app.set_config("--config", configPath, "Path to server.conf INI config", false);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        if (e.get_exit_code() == 0) {
            // 若为 --help 导致正常退出，直接向用户展示美化版帮助文本
            std::cout << app.help() << std::endl;
        } else {
            outError = "CLI11 parse error: " + std::string(e.what());
        }
        return false;
    }

    // 4. 解析完成后的最终依赖与路径组装
    out.serverRoot = serverRoot;
    out.configPath = configPath;
    out.baseDir = serverRoot + "assets/";
    out.logPath = serverRoot + "log/server.log";

    // 证书和私钥若为相对路径，自动转换为相对于 serverRoot 的路径
    if (out.enableSsl) {
        if (!out.sslCertPath.empty() && out.sslCertPath[0] != '/') {
            out.sslCertPath = serverRoot + out.sslCertPath;
        }
        if (!out.sslKeyPath.empty() && out.sslKeyPath[0] != '/') {
            out.sslKeyPath = serverRoot + out.sslKeyPath;
        }
    }

    // 确保日志目录存在
    const std::string logDir = serverRoot + "log/";
    if (!ensure_dir_recursive(logDir)) {
        std::cerr << "Warning: failed to ensure log directory: " << logDir << std::endl;
    }

    return true;
}
