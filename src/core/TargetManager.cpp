#include "core/TargetManager.hpp"
#include "logger.hpp"

namespace edgemon {

TargetManager::TargetManager()
    : running_(false)
    , initialized_(false)
    , nextTargetIndex_(1) {
    // Single canonical registry: TargetManager, the app, tests and the GUI all
    // observe the same TargetRegistry::instance() so target identity is
    // consistent process-wide.
    registry_ = std::shared_ptr<TargetRegistry>(
        &TargetRegistry::instance(), [](TargetRegistry*) {});
}

TargetManager::~TargetManager() {
    // Always detach the state observer, even if shutdown() was never called or
    // already ran: a lingering observer would capture a destroyed `this`.
    if (observerRegistered_) {
        registry_->removeStateObserver(stateObserverId_);
        observerRegistered_ = false;
    }
    shutdown();
}

void TargetManager::initialize() {
    if (initialized_.exchange(true)) {
        LOG_WARN("TargetManager already initialized");
        return;
    }

    discovery_ = std::make_shared<EdgeDiscovery>();
    resolver_ = std::make_shared<TargetResolver>();
    
    stateObserverId_ = registry_->observeStateChanges(
        [this](const TargetId& id, TargetState oldState, TargetState newState) {
            onTargetStateChanged(id, oldState, newState);
        });
    observerRegistered_ = true;
    
    running_ = true;
    LOG_INFO("TargetManager initialized");
}

void TargetManager::shutdown() {
    if (!running_.exchange(false)) {
        return;
    }

    // Detach the state observer first so a destroyed manager can never be
    // invoked through the singleton registry (use-after-free guard).
    if (observerRegistered_) {
        registry_->removeStateObserver(stateObserverId_);
        observerRegistered_ = false;
    }
    
    auto targets = registry_->getAll();
    for (const auto& target : targets) {
        stopMonitoring(target.id);
    }
    
    LOG_INFO("TargetManager shutdown complete");
}

TargetId TargetManager::lockTarget(const EdgeTarget& edgeTarget) {
    if (!initialized_) {
        LOG_ERROR("TargetManager not initialized");
        return TargetId();
    }

    if (findMatchingLockedTarget(edgeTarget)) {
        LOG_WARN("Target already locked for CDP: {}", edgeTarget.id.value);
        return *findMatchingLockedTarget(edgeTarget);
    }

    // URL-based duplicate prevention: the same page must not be locked twice.
    if (!edgeTarget.url.empty()) {
        if (auto byUrl = registry_->getByUrl(edgeTarget.url); byUrl) {
            LOG_WARN("Target already locked for URL: {}", edgeTarget.url.value);
            return byUrl->id;
        }
    }

    // Generate an internal id that does not collide with persisted/restored
    // targets (nextTargetIndex_ resets each process, so skip taken ids).
    TargetId internalId;
    do {
        internalId = TargetId("target-" + std::to_string(nextTargetIndex_++));
    } while (registry_->exists(internalId));
    
    MonitoredTarget target;
    target.id = internalId;
    target.cdpTargetId = edgeTarget.id;
    target.edgeProcessId = edgeTarget.processId;
    target.url = edgeTarget.url;
    target.title = edgeTarget.title;
    target.enabled = true;
    target.config.domEnabled = true;
    target.config.visualEnabled = true;
    target.config.ocrEnabled = false;
    target.config.mode = CaptureMode::Hybrid;
    target.state = TargetState::Connected;
    
    if (!registry_->add(target)) {
        LOG_ERROR("Failed to add target {} to registry", internalId.value);
        return TargetId();
    }
    
    LOG_INFO("Locked target {} -> CDP:{} URL:{}", 
             internalId.value, edgeTarget.id.value, edgeTarget.url.value);
    
    startMonitoring(internalId);
    
    return internalId;
}

TargetId TargetManager::lockTarget(const PageUrl& url) {
    if (!initialized_) {
        LOG_ERROR("TargetManager not initialized");
        return TargetId();
    }

    auto result = resolver_->resolve(url);
    if (!result.found) {
        LOG_ERROR("Cannot resolve URL: {}", url.value);
        return TargetId();
    }
    
    return lockTarget(result.target);
}

bool TargetManager::unlockTarget(const TargetId& id) {
    auto target = registry_->get(id);
    if (!target) {
        LOG_WARN("Target {} not found", id.value);
        return false;
    }
    
    stopMonitoring(id);
    
    if (!registry_->remove(id)) {
        return false;
    }
    
    LOG_INFO("Unlocked target {}", id.value);
    return true;
}

bool TargetManager::unlockAllTargets() {
    auto targets = registry_->getAll();
    bool success = true;
    
    for (const auto& target : targets) {
        if (!unlockTarget(target.id)) {
            success = false;
        }
    }
    
    LOG_INFO("Unlocked all targets");
    return success;
}

bool TargetManager::enableTarget(const TargetId& id) {
    if (!registry_->setEnabled(id, true)) {
        return false;
    }
    
    auto target = registry_->get(id);
    if (target && target->state == TargetState::Offline) {
        markTargetOnline(id);
    }
    
    startMonitoring(id);
    LOG_DEBUG("Target {} enabled", id.value);
    return true;
}

bool TargetManager::disableTarget(const TargetId& id) {
    if (!registry_->setEnabled(id, false)) {
        return false;
    }
    
    stopMonitoring(id);
    LOG_DEBUG("Target {} disabled", id.value);
    return true;
}

bool TargetManager::setMonitoringConfig(const TargetId& id, const MonitoringConfig& config) {
    auto target = registry_->get(id);
    if (!target) {
        return false;
    }
    
    target->config = config;
    return registry_->update(*target);
}

void TargetManager::setTargetCallback(TargetCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    targetCallbacks_.push_back(std::move(callback));
}

void TargetManager::setDiscoveryCallback(std::function<void(const std::vector<EdgeTarget>&)> callback) {
    discoveryCallback_ = std::move(callback);
}

void TargetManager::discoverTargets() {
    if (!running_) return;
    
    auto result = discovery_->discover();
    
    if (result.success) {
        resolver_->setInstances(result.instances);
        
        std::vector<EdgeTarget> pageTargets;
        for (const auto& target : result.targets) {
            if (target.isPage() || target.isBackgroundPage()) {
                pageTargets.push_back(target);
            }
        }
        
        if (discoveryCallback_) {
            discoveryCallback_(pageTargets);
        }
        
        LOG_INFO("Discovered {} page targets", pageTargets.size());
    } else {
        LOG_WARN("Discovery failed: {}", result.errorMessage);
    }
}

void TargetManager::refreshTarget(const TargetId& id) {
    auto target = registry_->get(id);
    if (!target) {
        return;
    }
    
    if (target->cdpTargetId.empty()) {
        return;
    }
    
    // Re-run discovery so we resolve against the *actual* Edge instance,
    // then re-bind the locked target's CDP id / websocket URL if it moved
    // (e.g. after a CDP reconnect or Edge restart).
    auto result = discovery_->discover();
    if (result.success) {
        resolver_->setInstances(result.instances);
    }

    for (const auto& instance : result.instances) {
        auto available = discovery_->findTargetById(instance, target->cdpTargetId);
        if (available) {
            MonitoredTarget updated = *target;
            updated.webSocketUrl = available->webSocketDebuggerUrl;
            updated.title = available->title;
            updated.edgeProcessId = instance.processId;
            if (!available->url.empty()) {
                updated.url = available->url;
            }
            registry_->update(updated);
            LOG_INFO("Target {} re-resolved: CDP {} on PID {}",
                     id.value, updated.cdpTargetId.value, updated.edgeProcessId.value);
            return;
        }
    }

    LOG_WARN("Target {} CDP target {} no longer available", 
             id.value, target->cdpTargetId.value);
    markTargetOffline(id, "CDP target not found during refresh");
}

void TargetManager::refreshAllTargets() {
    auto targets = registry_->getAll();
    for (const auto& target : targets) {
        refreshTarget(target.id);
    }
}

std::vector<MonitoredTarget> TargetManager::getLockedTargets() const {
    return registry_->getAll();
}

std::vector<MonitoredTarget> TargetManager::getAvailableTargets() const {
    return registry_->getEnabled();
}

std::optional<MonitoredTarget> TargetManager::getLockedTarget(const TargetId& id) const {
    return registry_->get(id);
}

std::optional<TargetId> TargetManager::findLockedTargetByUrl(const PageUrl& url) const {
    auto targets = registry_->getAll();
    for (const auto& target : targets) {
        if (target.url == url) {
            return target.id;
        }
    }
    return std::nullopt;
}

size_t TargetManager::lockedCount() const {
    return registry_->count();
}

size_t TargetManager::onlineCount() const {
    return registry_->countByState(TargetState::Monitoring) +
           registry_->countByState(TargetState::Connected);
}

TargetManager::TargetStats TargetManager::getStats() const {
    TargetStats stats;
    
    stats.totalLocked = registry_->count();
    stats.online = registry_->countByState(TargetState::Connected) +
                   registry_->countByState(TargetState::Monitoring);
    stats.offline = registry_->countByState(TargetState::Offline);
    stats.monitoring = registry_->countByState(TargetState::Monitoring);
    stats.error = registry_->countByState(TargetState::Error);
    
    return stats;
}

void TargetManager::markTargetOffline(const TargetId& id, const std::string& reason) {
    registry_->setState(id, TargetState::Offline, reason);
    stopMonitoring(id);
    LOG_WARN("Target {} marked OFFLINE: {}", id.value, reason);
}

void TargetManager::markTargetOnline(const TargetId& id) {
    registry_->setState(id, TargetState::Connected);
    startMonitoring(id);
    LOG_INFO("Target {} marked ONLINE", id.value);
}

void TargetManager::handleTargetLost(const CDPTargetId& cdpId) {
    auto target = registry_->getByCDPId(cdpId);
    if (!target) {
        return;
    }
    
    LOG_WARN("CDP target {} lost for locked target {}", 
             cdpId.value, target->id.value);
    markTargetOffline(target->id, "CDP target lost");
}

void TargetManager::handleTargetDiscovered(const EdgeTarget& edgeTarget) {
    auto existingLock = findMatchingLockedTarget(edgeTarget);
    
    if (!existingLock) {
        LOG_DEBUG("New available target: {} - {}", 
                 edgeTarget.title, edgeTarget.url.value);
    }
}

void TargetManager::startMonitoring(const TargetId& id) {
    auto target = registry_->get(id);
    if (!target || !target->enabled) {
        return;
    }
    
    registry_->setState(id, TargetState::Monitoring);
    LOG_DEBUG("Started monitoring target {}", id.value);
}

void TargetManager::stopMonitoring(const TargetId& id) {
    auto target = registry_->get(id);
    if (target && target->state == TargetState::Monitoring) {
        registry_->setState(id, TargetState::Connected);
    }
    LOG_DEBUG("Stopped monitoring target {}", id.value);
}

void TargetManager::reconnectTarget(const TargetId& id) {
    auto target = registry_->get(id);
    if (!target) {
        return;
    }
    
    LOG_INFO("Reconnecting target {}", id.value);
    registry_->setState(id, TargetState::Reconnecting);
    
    refreshTarget(id);
}

std::optional<TargetId> TargetManager::findMatchingLockedTarget(const EdgeTarget& edgeTarget) const {
    auto lockedTargets = registry_->getAll();
    
    for (const auto& locked : lockedTargets) {
        if (isTargetMatch(locked, edgeTarget)) {
            return locked.id;
        }
    }
    
    return std::nullopt;
}

bool TargetManager::isTargetMatch(const MonitoredTarget& locked, const EdgeTarget& available) const {
    if (!locked.cdpTargetId.empty() && locked.cdpTargetId == available.id) {
        return true;
    }
    
    if (!locked.url.empty() && !available.url.empty() && 
        locked.url == available.url) {
        return true;
    }
    
    return false;
}

void TargetManager::onTargetStateChanged(const TargetId& id, TargetState oldState, TargetState newState) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    
    auto target = registry_->get(id);
    if (!target) {
        return;
    }
    
    for (const auto& callback : targetCallbacks_) {
        try {
            callback(*target, newState);
        } catch (const std::exception& e) {
            LOG_ERROR("Target callback exception: {}", e.what());
        }
    }
}

void TargetManager::onDiscoveryComplete(const std::vector<EdgeTarget>& targets) {
    if (discoveryCallback_) {
        discoveryCallback_(targets);
    }
    
    for (const auto& target : targets) {
        handleTargetDiscovered(target);
    }
}

} // namespace edgemon
