#pragma once

#include "common.hpp"
#include <vector>
#include <string>
#include <optional>

namespace edgemon {

struct EdgeInstance {
    int processId;
    std::string userDataDir;
    int debugPort;
    std::string browserVersion;
    std::vector<std::string> targetIds;
};

class EdgeDiscovery {
public:
    static std::vector<EdgeInstance> discoverAll();
    static std::optional<EdgeInstance> discoverByUrl(const std::string& url);
    static std::vector<EdgeInstance> discoverByTitle(const std::string& title);
    static std::optional<EdgeInstance> discoverByProcessId(int pid);
    static bool isEdgeRunning();
    static std::string getWebSocketDebuggerUrl(int debugPort);
};

}
