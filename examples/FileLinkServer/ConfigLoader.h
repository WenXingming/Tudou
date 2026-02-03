#pragma once

#include <string>

#include "FileLinkServer.h"

struct FileLinkServerBootstrap {
    FileLinkServerConfig cfg;
    std::string serverRoot;   // ends with '/'
    std::string configPath;   // {serverRoot}conf/server.conf
    std::string logPath;      // {serverRoot}log/server.log
};

// Loads serverRoot + server.conf and fills cfg.
// - If argc > 1: argv[1] is treated as serverRoot.
// - Else: searches a few default roots.
// Returns true on success; on failure returns false and sets outError.
bool load_filelink_server_bootstrap(int argc, char* argv[], FileLinkServerBootstrap& out, std::string& outError);
