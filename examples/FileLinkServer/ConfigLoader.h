#pragma once

#include <string>

#include "FileLinkServerConfig.h"

// Loads serverRoot + server.conf and fills cfg.
// - If "-r <serverRoot>" or "--root <serverRoot>" is provided: uses that.
// - Else if argc > 1 and argv[1] doesn't look like an option: argv[1] is treated as serverRoot.
// - Else: searches a few default roots.
// Returns true on success; on failure returns false and sets outError.
bool load_filelink_server_config(int argc, char* argv[], FileLinkServerConfig& out, std::string& outError);
