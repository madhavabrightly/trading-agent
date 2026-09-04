#pragma once

#include "core/Types.hpp"
#include "core/TargetRegistry.hpp"
#include "edge/EdgeDiscovery.hpp"
#include "edge/TargetResolver.hpp"
#include <cstdint>
#include <memory>
#include <atomic>
#include <vector>
#include <functional>
#include <optional>

namespace edgemon {

class TargetManager : public std::enable_shared_from_this<TargetManager> {
public:
    using TargetCallback = std::function<void(const MonitoredTarget&, TargetState)>;

    explicit TargetManager();
    ~TargetManager();

    TargetManager(const TargetManager&) = delete;
    TargetManager& operator=(const TargetManager&) = delete;

    void initialize();
    void shutdown();

    TargetId lockTarget(const EdgeTarget& edgeTarget);
    TargetId lockTarget(const PageUrl& url);
    
    bool unlockTarget(const TargetId& id);
    bool unlockAllTargets();

    bool enableTarget(const TargetId& id);
    bool disableTarget(const TargetId& id);
    
    bool setMonitoringConfig(const TargetId& id, const MonitoringConfig& config);

    void setTargetCallback(TargetCallback callback);
    void setDiscoveryCallback(std::function<void(const std::vector<EdgeTarget>&)> callback);

    void discoverTargets();
    void refreshTarget(const TargetId& id);
    void refreshAllTargets();

    std::vector<MonitoredTarget> getLockedTargets() const;
    std::vector<MonitoredTarget> getAvailableTargets() const;
    
    std::optional<MonitoredTarget> getLockedTarget(const TargetId& id) const;
    
    std::optional<TargetId> findLockedTargetByUrl(const PageUrl& url) const;
    
    size_t lockedCount() const;
    size_t onlineCount() const;

    struct TargetStats {
        size_t totalLocked = 0;
        size_t online = 0;
        size_t offline = 0;
        size_t monitoring = 0;
        size_t error = 0;
    };
    
    TargetStats getStats() const;

    void markTargetOffline(const TargetId& id, const std::string& reason = "");
    void markTargetOnline(const TargetId& id);
    
    void handleTargetLost(const CDPTargetId& cdpId);
    void handleTargetDiscovered(const EdgeTarget& edgeTarget);

private:
    void startMonitoring(const TargetId& id);
    void stopMonitoring(const TargetId& id);
    void reconnectTarget(const TargetId& id);
    
    std::optional<TargetId> findMatchingLockedTarget(const EdgeTarget& edgeTarget) const;
    bool isTargetMatch(const MonitoredTarget& locked, const EdgeTarget& available) const;

    void onTargetStateChanged(const TargetId& id, TargetState oldState, TargetState newState);
    void onDiscoveryComplete(const std::vector<EdgeTarget>& targets);

    std::shared_ptr<TargetRegistry> registry_;
    std::shared_ptr<EdgeDiscovery> discovery_;
    std::shared_ptr<TargetResolver> resolver_;
    
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    
    std::vector<TargetCallback> targetCallbacks_;
    std::function<void(const std::vector<EdgeTarget>&)> discoveryCallback_;
    
    mutable std::mutex callbackMutex_;
    
    std::atomic<uint32_t> nextTargetIndex_;
    size_t stateObserverId_ = 0;
    bool observerRegistered_ = false;
};

} // namespace edgemon
