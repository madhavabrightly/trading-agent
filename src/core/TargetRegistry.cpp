#include "core/TargetRegistry.hpp"
#include "logger.hpp"
#include <algorithm>

namespace edgemon {

TargetRegistry& TargetRegistry::instance() {
    // Heap-allocated, deliberately never freed: the registry is referenced by
    // TargetManager's non-owning shared_ptr and by tests; destroying it before
    // those references (static destruction order at exit) would be a
    // use-after-free.
    static TargetRegistry* registry = new TargetRegistry();
    return *registry;
}

bool TargetRegistry::add(const MonitoredTarget& target) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (targets_.contains(target.id)) {
        LOG_WARN("Target {} already exists in registry", target.id.value);
        return false;
    }
    
    if (!target.cdpTargetId.empty() && cdpIndex_.contains(target.cdpTargetId)) {
        LOG_WARN("CDP target {} already registered", target.cdpTargetId.value);
        return false;
    }
    
    targets_[target.id] = target;
    
    if (!target.cdpTargetId.empty()) {
        cdpIndex_[target.cdpTargetId] = target.id;
    }
    
    LOG_INFO("Target {} added to registry (CDP: {}, URL: {})", 
             target.id.value, target.cdpTargetId.value, target.url.value);
    return true;
}

bool TargetRegistry::remove(const TargetId& id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = targets_.find(id);
    if (it == targets_.end()) {
        return false;
    }
    
    const auto& target = it->second;
    if (!target.cdpTargetId.empty()) {
        cdpIndex_.erase(target.cdpTargetId);
    }
    
    targets_.erase(it);
    LOG_INFO("Target {} removed from registry", id.value);
    return true;
}

bool TargetRegistry::update(const MonitoredTarget& target) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = targets_.find(target.id);
    if (it == targets_.end()) {
        LOG_WARN("Cannot update non-existent target {}", target.id.value);
        return false;
    }
    
    MonitoredTarget oldTarget = it->second;
    it->second = target;
    
    if (oldTarget.cdpTargetId != target.cdpTargetId) {
        if (!oldTarget.cdpTargetId.empty()) {
            cdpIndex_.erase(oldTarget.cdpTargetId);
        }
        if (!target.cdpTargetId.empty()) {
            cdpIndex_[target.cdpTargetId] = target.id;
        }
    }
    
    return true;
}

std::optional<MonitoredTarget> TargetRegistry::get(const TargetId& id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = targets_.find(id);
    if (it != targets_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<MonitoredTarget> TargetRegistry::getByCDPId(const CDPTargetId& cdpId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto cdpIt = cdpIndex_.find(cdpId);
    if (cdpIt != cdpIndex_.end()) {
        auto it = targets_.find(cdpIt->second);
        if (it != targets_.end()) {
            return it->second;
        }
    }
    return std::nullopt;
}

std::optional<MonitoredTarget> TargetRegistry::getByUrl(const PageUrl& url) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    for (const auto& [id, target] : targets_) {
        if (target.url.value == url.value) {
            return target;
        }
    }
    return std::nullopt;
}

std::vector<MonitoredTarget> TargetRegistry::getAll() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<MonitoredTarget> result;
    result.reserve(targets_.size());
    for (const auto& [id, target] : targets_) {
        result.push_back(target);
    }
    return result;
}

std::vector<MonitoredTarget> TargetRegistry::getEnabled() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<MonitoredTarget> result;
    for (const auto& [id, target] : targets_) {
        if (target.enabled) {
            result.push_back(target);
        }
    }
    return result;
}

std::vector<MonitoredTarget> TargetRegistry::getByState(TargetState state) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<MonitoredTarget> result;
    for (const auto& [id, target] : targets_) {
        if (target.state == state) {
            result.push_back(target);
        }
    }
    return result;
}

bool TargetRegistry::setState(const TargetId& id, TargetState state) {
    return setState(id, state, std::string());
}

bool TargetRegistry::setState(const TargetId& id, TargetState state, const std::string& message) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = targets_.find(id);
    if (it == targets_.end()) {
        return false;
    }
    
    TargetState oldState = it->second.state;
    it->second.state = state;
    it->second.stateMessage = message;
    
    if (!message.empty()) {
        it->second.stateMessage = message;
    }
    
    lock.unlock();
    
    if (oldState != state) {
        std::lock_guard<std::mutex> obsLock(observerMutex_);
        for (const auto& observer : stateObservers_) {
            if (!observer) continue;
            try {
                observer(id, oldState, state);
            } catch (const std::exception& e) {
                LOG_ERROR("State observer exception: {}", e.what());
            }
        }
    }
    
    LOG_DEBUG("Target {} state: {} -> {}", id.value, 
              TargetStateToString(oldState), TargetStateToString(state));
    return true;
}

bool TargetRegistry::setEnabled(const TargetId& id, bool enabled) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = targets_.find(id);
    if (it == targets_.end()) {
        return false;
    }
    
    it->second.enabled = enabled;
    LOG_DEBUG("Target {} enabled: {}", id.value, enabled);
    return true;
}

bool TargetRegistry::setCDPTargetId(const TargetId& id, const CDPTargetId& cdpId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = targets_.find(id);
    if (it == targets_.end()) {
        return false;
    }
    
    if (!it->second.cdpTargetId.empty()) {
        cdpIndex_.erase(it->second.cdpTargetId);
    }
    
    if (!cdpId.empty() && cdpIndex_.contains(cdpId)) {
        LOG_WARN("CDP target {} already registered to another target", cdpId.value);
        return false;
    }
    
    it->second.cdpTargetId = cdpId;
    if (!cdpId.empty()) {
        cdpIndex_[cdpId] = id;
    }
    
    LOG_DEBUG("Target {} CDP ID: {}", id.value, cdpId.value);
    return true;
}

void TargetRegistry::updateLastHash(const TargetId& id, uint64_t domHash, uint64_t visualHash) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = targets_.find(id);
    if (it != targets_.end()) {
        it->second.lastDomHash = domHash;
        it->second.lastVisualHash = visualHash;
        it->second.lastChangeTimestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }
}

bool TargetRegistry::exists(const TargetId& id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return targets_.contains(id);
}

bool TargetRegistry::existsByCDPId(const CDPTargetId& cdpId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return cdpIndex_.contains(cdpId);
}

size_t TargetRegistry::count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return targets_.size();
}

size_t TargetRegistry::countByState(TargetState state) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return std::count_if(targets_.begin(), targets_.end(),
        [state](const auto& pair) { return pair.second.state == state; });
}

void TargetRegistry::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    targets_.clear();
    cdpIndex_.clear();
    LOG_INFO("Target registry cleared");
}

void TargetRegistry::removeByCDPTargetId(const CDPTargetId& cdpId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto cdpIt = cdpIndex_.find(cdpId);
    if (cdpIt != cdpIndex_.end()) {
        auto targetIt = targets_.find(cdpIt->second);
        if (targetIt != targets_.end()) {
            targets_.erase(targetIt);
        }
        cdpIndex_.erase(cdpIt);
    }
}

size_t TargetRegistry::observeStateChanges(StateObserver observer) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    size_t id = stateObservers_.size();
    stateObservers_.push_back(std::move(observer));
    return id;
}

bool TargetRegistry::removeStateObserver(size_t observerId) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    if (observerId >= stateObservers_.size()) {
        return false;
    }
    stateObservers_[observerId] = nullptr;
    return true;
}

} // namespace edgemon
