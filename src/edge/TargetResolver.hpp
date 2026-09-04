#pragma once

#include "common.hpp"
#include "CDPClient.hpp"
#include <string>
#include <optional>

namespace edgemon {

struct ResolvedTarget {
    std::string targetId;
    std::string url;
    std::string title;
    std::string type;
    int processId;
};

class TargetResolver {
public:
    static std::vector<ResolvedTarget> listAllTargets(int debugPort);
    static std::optional<ResolvedTarget> findByUrl(int debugPort, const std::string& url);
    static std::optional<ResolvedTarget> findByTitle(int debugPort, const std::string& title);
    static std::optional<ResolvedTarget> findByProcessId(int debugPort, int pid);
    static std::string resolveTargetId(int debugPort, const std::string& url);
};

}
