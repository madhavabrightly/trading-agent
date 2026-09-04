#pragma once

#include "core/Types.hpp"
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <vector>
#include <optional>
#include <functional>

namespace edgemon {

class TargetRegistry {
public:
    static TargetRegistry& instance();

    bool add(const MonitoredTarget& target);
    bool remove(const TargetId& id);
    bool update(const MonitoredTarget& target);
    
    std::optional<MonitoredTarget> get(const TargetId& id) const;
    std::optional<MonitoredTarget> getByCDPId(const CDPTargetId& cdpId) const;
    std::optional<MonitoredTarget> getByUrl(const PageUrl& url) const;
    
    std::vector<MonitoredTarget> getAll() const;
    std::vector<MonitoredTarget> getEnabled() const;
    std::vector<MonitoredTarget> getByState(TargetState state) const;
    
    bool setState(const TargetId& id, TargetState state);
    bool setState(const TargetId& id, TargetState state, const std::string& message);
    
    bool setEnabled(const TargetId& id, bool enabled);
    bool setCDPTargetId(const TargetId& id, const CDPTargetId& cdpId);
    
    void updateLastHash(const TargetId& id, uint64_t domHash, uint64_t visualHash);
    
    bool exists(const TargetId& id) const;
    bool existsByCDPId(const CDPTargetId& cdpId) const;
    
    size_t count() const;
    size_t countByState(TargetState state) const;
    
    void clear();
    void removeByCDPTargetId(const CDPTargetId& cdpId);

    using StateObserver = std::function<void(const TargetId&, TargetState, TargetState)>;
    size_t observeStateChanges(StateObserver observer);
    bool removeStateObserver(size_t observerId);

    TargetRegistry() = default;
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<TargetId, MonitoredTarget, std::hash<TargetId>> targets_;
    std::unordered_map<CDPTargetId, TargetId, std::hash<CDPTargetId>> cdpIndex_;
    
    std::vector<StateObserver> stateObservers_;
    mutable std::mutex observerMutex_;
};

} // namespace edgemon
