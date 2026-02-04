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
// - If "-r <serverRoot>" or "--root <serverRoot>" is provided: uses that.
// - Else if argc > 1 and argv[1] doesn't look like an option: argv[1] is treated as serverRoot.
// - Else: searches a few default roots.
// Returns true on success; on failure returns false and sets outError.
bool load_filelink_server_bootstrap(int argc, char* argv[], FileLinkServerBootstrap& out, std::string& outError);
