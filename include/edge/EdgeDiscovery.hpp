#pragma once

#include "core/Types.hpp"
#include <vector>
#include <string>
#include <optional>

namespace edgemon {

class EdgeDiscovery {
public:
    struct DiscoveryConfig {
        std::vector<int> preferredPorts = {9222, 9223, 9224, 9225, 9226, 9227, 9228, 9229, 9230};
        int timeoutMs = 800;
        bool autoDiscover = true;
    };

    EdgeDiscovery() = default;
    explicit EdgeDiscovery(const DiscoveryConfig& config);
    ~EdgeDiscovery() = default;

    void setConfig(const DiscoveryConfig& config);

    std::vector<EdgeInstance> discoverAllInstances();
    
    std::optional<EdgeInstance> discoverByProcessId(EdgeProcessId pid);
    std::vector<EdgeInstance> discoverByPort(int port);

    std::vector<EdgeTarget> enumerateTargets(const EdgeInstance& instance);
    std::vector<EdgeTarget> enumerateAllTargets();

    std::optional<EdgeTarget> findTargetByUrl(const EdgeInstance& instance, const PageUrl& url);
    std::optional<EdgeTarget> findTargetByTitle(const EdgeInstance& instance, const std::string& title);
    std::optional<EdgeTarget> findTargetById(const EdgeInstance& instance, const CDPTargetId& id);

    bool isEdgeRunning();
    std::string buildWebSocketUrl(const EdgeInstance& instance, const CDPTargetId& targetId);

    struct DiscoveryResult {
        bool success = false;
        std::string errorMessage;
        std::vector<EdgeInstance> instances;
        std::vector<EdgeTarget> targets;
    };

    DiscoveryResult discover();

private:
    std::optional<EdgeInstance> probePort(int port);
    bool probeEndpoint(const std::string& url);
    std::vector<EdgeTarget> parseTargetList(const std::string& jsonResponse);
    
    DiscoveryConfig config_;
};

} // namespace edgemon
