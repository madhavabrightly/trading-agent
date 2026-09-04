#include "core/ReliabilityManager.hpp"
#include "logger.hpp"

namespace edgemon {

ReliabilityManager::ReliabilityManager() = default;

ReliabilityManager::~ReliabilityManager() {
    shutdown();
}

void ReliabilityManager::initialize() {
    if (running_.exchange(true)) return;

    reconnectThread_ = std::jthread([this](std::stop_token token) {
        while (!token.stop_requested()) {
            std::vector<TargetId> toReconnect;

            {
                std::lock_guard<std::mutex> lock(reconnectMutex_);
                auto now = std::chrono::steady_clock::now();

                for (auto it = pendingReconnects_.begin(); it != pendingReconnects_.end(); ) {
                    TargetReliability rel;
                    {
                        std::lock_guard<std::mutex> lock(targetsMutex_);
                        auto relIt = reliability_.find(*it);
                        if (relIt != reliability_.end()) {
                            rel = relIt->second;
                        }
                    }

                    if (rel.targetId.value.empty() || !rel.isRecoverable) {
                        it = pendingReconnects_.erase(it);
                        continue;
                    }

                    if (now - rel.lastReconnectAttempt >= rel.backoffMs) {
                        toReconnect.push_back(*it);
                        it = pendingReconnects_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            for (const auto& targetId : toReconnect) {
                if (token.stop_requested()) break;
                executeReconnect(targetId);
            }

            std::unique_lock<std::mutex> lock(reconnectMutex_);
            reconnectCV_.wait_for(lock, std::chrono::seconds(1), [token] {
                return token.stop_requested();
            });
        }
    });

    LOG_INFO("ReliabilityManager: initialized");
}

void ReliabilityManager::shutdown() {
    if (!running_.exchange(false)) return;

    if (reconnectThread_.joinable()) {
        reconnectThread_.request_stop();
        reconnectCV_.notify_all();
        reconnectThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(reconnectMutex_);
        pendingReconnects_.clear();
    }

    LOG_INFO("ReliabilityManager: shutdown complete");
}

void ReliabilityManager::registerTarget(const TargetId& targetId, std::shared_ptr<PageSession> session) {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    sessions_[targetId] = session;

    TargetReliability rel;
    rel.targetId = targetId;
    rel.currentState = TargetState::Connecting;
    rel.previousState = TargetState::Discovered;
    rel.lastStateChange = std::chrono::steady_clock::now();
    reliability_[targetId] = rel;

    LOG_INFO("ReliabilityManager: registered target {}", targetId.value);
}

void ReliabilityManager::unregisterTarget(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    sessions_.erase(targetId);
    reliability_.erase(targetId);

    {
        std::lock_guard<std::mutex> lock(reconnectMutex_);
        pendingReconnects_.erase(
            std::remove(pendingReconnects_.begin(), pendingReconnects_.end(), targetId),
            pendingReconnects_.end()
        );
    }

    LOG_INFO("ReliabilityManager: unregistered target {}", targetId.value);
}

void ReliabilityManager::setStateChangeCallback(StateChangeCallback callback) {
    stateChangeCallback_ = std::move(callback);
}

void ReliabilityManager::setErrorCallback(ErrorCallback callback) {
    errorCallback_ = std::move(callback);
}

void ReliabilityManager::setRecoveryCallback(RecoveryCallback callback) {
    recoveryCallback_ = std::move(callback);
}

ReliabilityManager::TargetReliability ReliabilityManager::getReliability(const TargetId& targetId) const {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    if (reliability_.contains(targetId)) {
        return reliability_.at(targetId);
    }
    return TargetReliability{targetId};
}

bool ReliabilityManager::isTargetHealthy(const TargetId& targetId) const {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    if (!reliability_.contains(targetId)) return false;

    const auto& rel = reliability_.at(targetId);
    return rel.currentState == TargetState::Connected ||
           rel.currentState == TargetState::Monitoring ||
           rel.currentState == TargetState::Navigating;
}

bool ReliabilityManager::shouldReconnect(const TargetId& targetId) const {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    if (!reliability_.contains(targetId)) return false;

    const auto& rel = reliability_.at(targetId);
    return rel.isRecoverable &&
           rel.reconnectAttempts < rel.maxReconnectAttempts &&
           (rel.currentState == TargetState::Reconnecting ||
            rel.currentState == TargetState::Offline ||
            rel.currentState == TargetState::Closed);
}

void ReliabilityManager::handleDisconnect(const TargetId& targetId) {
    LOG_WARN("ReliabilityManager: target {} disconnected", targetId.value);

    totalDisconnects_++;

    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (!reliability_.contains(targetId)) return;

        auto& rel = reliability_[targetId];
        rel.lastErrorTime = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
        rel.lastError = "Disconnected";
        rel.lastReconnectAttempt = std::chrono::steady_clock::now();

        transitionState(targetId, TargetState::Reconnecting);
    }

    scheduleReconnect(targetId);
}

void ReliabilityManager::handleReconnect(const TargetId& targetId) {
    LOG_INFO("ReliabilityManager: target {} reconnected", targetId.value);

    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (!reliability_.contains(targetId)) return;

        auto& rel = reliability_[targetId];
        rel.reconnectAttempts = 0;
        rel.backoffMs = std::chrono::milliseconds{250};
        rel.lastError.clear();

        transitionState(targetId, TargetState::Connected);
    }

    if (recoveryCallback_) {
        recoveryCallback_(targetId, true);
    }
}

void ReliabilityManager::handleCrash(const TargetId& targetId) {
    LOG_ERROR("ReliabilityManager: target {} crashed", targetId.value);

    crashes_++;
    totalDisconnects_++;

    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (!reliability_.contains(targetId)) return;

        auto& rel = reliability_[targetId];
        rel.lastErrorTime = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
        rel.lastError = "Browser crash";
        rel.lastReconnectAttempt = std::chrono::steady_clock::now();

        transitionState(targetId, TargetState::Reconnecting);
    }

    scheduleReconnect(targetId);
}

void ReliabilityManager::handleNavigation(const TargetId& targetId, const PageUrl& newUrl) {
    LOG_DEBUG("ReliabilityManager: target {} navigated to {}", targetId.value, newUrl.value);

    std::lock_guard<std::mutex> lock(targetsMutex_);
    if (!reliability_.contains(targetId)) return;

    transitionState(targetId, TargetState::Navigating);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (reliability_.at(targetId).currentState == TargetState::Navigating) {
        transitionState(targetId, TargetState::Connected);
    }
}

void ReliabilityManager::handleTabClose(const TargetId& targetId) {
    LOG_WARN("ReliabilityManager: target {} tab closed", targetId.value);

    tabsClosed_++;

    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (!reliability_.contains(targetId)) return;

        auto& rel = reliability_[targetId];
        rel.isRecoverable = false;
        rel.lastError = "Tab closed";
        rel.lastErrorTime = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );

        transitionState(targetId, TargetState::Closed);
    }

    {
        std::lock_guard<std::mutex> lock(reconnectMutex_);
        pendingReconnects_.erase(
            std::remove(pendingReconnects_.begin(), pendingReconnects_.end(), targetId),
            pendingReconnects_.end()
        );
    }
}

void ReliabilityManager::attemptReconnect(const TargetId& targetId) {
    scheduleReconnect(targetId);
}

void ReliabilityManager::cancelReconnect(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(reconnectMutex_);
    pendingReconnects_.erase(
        std::remove(pendingReconnects_.begin(), pendingReconnects_.end(), targetId),
        pendingReconnects_.end()
    );
    LOG_INFO("ReliabilityManager: cancelled reconnect for {}", targetId.value);
}

ReliabilityManager::ReliabilityStats ReliabilityManager::getStats() const {
    ReliabilityStats stats;
    stats.totalDisconnects = totalDisconnects_.load();
    stats.successfulReconnects = successfulReconnects_.load();
    stats.failedReconnects = failedReconnects_.load();
    stats.crashes = crashes_.load();
    stats.tabsClosed = tabsClosed_.load();

    std::lock_guard<std::mutex> lock(targetsMutex_);
    stats.activeTargets = static_cast<uint32_t>(reliability_.size());
    stats.healthyTargets = 0;
    for (const auto& [id, rel] : reliability_) {
        if (rel.currentState == TargetState::Connected ||
            rel.currentState == TargetState::Monitoring ||
            rel.currentState == TargetState::Navigating) {
            stats.healthyTargets++;
        }
    }

    return stats;
}

void ReliabilityManager::resetStats() {
    totalDisconnects_ = 0;
    successfulReconnects_ = 0;
    failedReconnects_ = 0;
    crashes_ = 0;
    tabsClosed_ = 0;
}

void ReliabilityManager::transitionState(const TargetId& targetId, TargetState newState) {
    if (!reliability_.contains(targetId)) return;

    auto& rel = reliability_[targetId];
    TargetState oldState = rel.currentState;

    if (oldState == newState) return;

    rel.previousState = oldState;
    rel.currentState = newState;
    rel.lastStateChange = std::chrono::steady_clock::now();

    LOG_DEBUG("ReliabilityManager: {} state {} -> {}", targetId.value,
              static_cast<int>(oldState), static_cast<int>(newState));

    if (stateChangeCallback_) {
        stateChangeCallback_(targetId, oldState, newState);
    }
}

void ReliabilityManager::scheduleReconnect(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    if (!reliability_.contains(targetId)) return;

    auto& rel = reliability_[targetId];

    if (!rel.isRecoverable || rel.reconnectAttempts >= rel.maxReconnectAttempts) {
        LOG_WARN("ReliabilityManager: {} not recoverable or max attempts reached", targetId.value);
        transitionState(targetId, TargetState::Offline);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(reconnectMutex_);
        if (std::find(pendingReconnects_.begin(), pendingReconnects_.end(), targetId) == pendingReconnects_.end()) {
            pendingReconnects_.push_back(targetId);
        }
    }

    reconnectCV_.notify_one();

    LOG_INFO("ReliabilityManager: scheduled reconnect {} (attempt {}/{})",
             targetId.value, rel.reconnectAttempts + 1, rel.maxReconnectAttempts);
}

void ReliabilityManager::executeReconnect(const TargetId& targetId) {
    LOG_INFO("ReliabilityManager: executing reconnect for {}", targetId.value);

    std::shared_ptr<PageSession> session;
    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (sessions_.contains(targetId)) {
            session = sessions_[targetId];
        }
    }

    if (!session) {
        LOG_ERROR("ReliabilityManager: no session for {}", targetId.value);
        failedReconnects_++;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (reliability_.contains(targetId)) {
            reliability_[targetId].reconnectAttempts++;
            reliability_[targetId].lastReconnectAttempt = std::chrono::steady_clock::now();
        }
    }

    try {
        bool connected = session->connect();

        if (connected) {
            onReconnectComplete(targetId, true);
        } else {
            onReconnectComplete(targetId, false);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("ReliabilityManager: reconnect exception for {}: {}", targetId.value, e.what());
        onReconnectComplete(targetId, false);
    }
}

void ReliabilityManager::onReconnectComplete(const TargetId& targetId, bool success) {
    if (success) {
        LOG_INFO("ReliabilityManager: {} reconnected successfully", targetId.value);
        successfulReconnects_++;

        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (reliability_.contains(targetId)) {
            auto& rel = reliability_[targetId];
            rel.reconnectAttempts = 0;
            rel.backoffMs = std::chrono::milliseconds{250};
            rel.lastError.clear();
            transitionState(targetId, TargetState::Connected);
        }
    } else {
        LOG_WARN("ReliabilityManager: {} reconnect failed", targetId.value);
        failedReconnects_++;

        std::lock_guard<std::mutex> lock(targetsMutex_);
        if (reliability_.contains(targetId)) {
            auto& rel = reliability_[targetId];
            rel.backoffMs = calculateBackoff(rel.reconnectAttempts);

            if (rel.reconnectAttempts >= rel.maxReconnectAttempts) {
                rel.isRecoverable = false;
                transitionState(targetId, TargetState::Offline);
                LOG_ERROR("ReliabilityManager: {} max reconnect attempts reached", targetId.value);
            } else {
                scheduleReconnect(targetId);
            }
        }
    }

    if (recoveryCallback_) {
        recoveryCallback_(targetId, success);
    }
}

std::chrono::milliseconds ReliabilityManager::calculateBackoff(uint32_t attempts) {
    std::chrono::milliseconds base{250};
    std::chrono::milliseconds max{30000};

    uint64_t backoff = static_cast<uint64_t>(base.count()) * (1ULL << std::min(attempts, 7u));
    backoff = std::min(backoff, static_cast<uint64_t>(max.count()));

    return std::chrono::milliseconds(backoff);
}

} // namespace edgemon
