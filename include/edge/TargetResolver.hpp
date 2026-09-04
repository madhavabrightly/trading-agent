#pragma once

#include "core/Types.hpp"
#include "edge/EdgeDiscovery.hpp"
#include "edge/UrlUtils.hpp"
#include <vector>
#include <optional>

namespace edgemon {

class TargetResolver {
public:
    TargetResolver() = default;
    ~TargetResolver() = default;

    void addInstance(const EdgeInstance& instance);
    void setInstances(const std::vector<EdgeInstance>& instances);
    void clearInstances();

    std::optional<CDPTargetId> resolveByUrl(const PageUrl& url);
    std::optional<CDPTargetId> resolveByTitle(const std::string& title);
    std::optional<CDPTargetId> resolveByProcessId(EdgeProcessId pid);
    
    std::optional<EdgeTarget> resolveTargetByUrl(const PageUrl& url);
    std::optional<EdgeTarget> resolveTargetByTitle(const std::string& title);
    
    struct ResolutionResult {
        bool found = false;
        bool notFound = false;   // deterministic TARGET_NOT_FOUND (no match)
        CDPTargetId cdpTargetId;
        EdgeInstance instance;
        EdgeTarget target;
        TargetMatchRule rule = TargetMatchRule::Exact;
        std::string errorMessage;
    };
    
    ResolutionResult resolve(const PageUrl& url);
    ResolutionResult resolve(const PageUrl& url, TargetMatchRule rule);
    ResolutionResult resolveByExactMatch(const PageUrl& url);

    std::vector<EdgeTarget> getAllTargets();
    std::vector<EdgeInstance> getInstances();

    // Deterministic candidate enumeration + selection (exact page targets only).
    static std::vector<EdgeTarget> filterPageTargets(
        const std::vector<EdgeTarget>& targets);

private:
    std::optional<EdgeInstance> findInstanceForTarget(const EdgeTarget& target);
    void refreshTargets();
    ResolutionResult resolveImpl(const PageUrl& url, TargetMatchRule rule);

    std::vector<EdgeInstance> instances_;
    std::vector<EdgeTarget> cachedTargets_;
    EdgeDiscovery discovery_;
};

} // namespace edgemon
