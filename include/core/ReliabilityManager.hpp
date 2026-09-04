#pragma once

#include "core/Types.hpp"
#include "core/TargetManager.hpp"
#include "edge/PageSession.hpp"
#include <memory>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <mutex>
#include <chrono>

namespace edgemon {

class ReliabilityManager : public std::enable_shared_from_this<ReliabilityManager> {
public:
    using StateChangeCallback = std::function<void(const TargetId&, TargetState, TargetState)>;
    using ErrorCallback = std::function<void(const TargetId&, const std::string&, bool recoverable)>;
    using RecoveryCallback = std::function<void(const TargetId&, bool success)>;

    explicit ReliabilityManager();
    ~ReliabilityManager();

    void initialize();
    void shutdown();

    void registerTarget(const TargetId& targetId, std::shared_ptr<PageSession> session);
    void unregisterTarget(const TargetId& targetId);

    void setStateChangeCallback(StateChangeCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void setRecoveryCallback(RecoveryCallback callback);

    struct TargetReliability {
        TargetId targetId;
        TargetState currentState = TargetState::Discovered;
        TargetState previousState = TargetState::Discovered;
        uint32_t reconnectAttempts = 0;
        uint32_t maxReconnectAttempts = 5;
        uint64_t lastErrorTime = 0;
        std::string lastError;
        bool isRecoverable = true;
        std::chrono::milliseconds backoffMs{250};
        std::chrono::steady_clock::time_point lastStateChange;
        std::chrono::steady_clock::time_point lastReconnectAttempt;
    };

    TargetReliability getReliability(const TargetId& targetId) const;
    bool isTargetHealthy(const TargetId& targetId) const;
    bool shouldReconnect(const TargetId& targetId) const;

    void handleDisconnect(const TargetId& targetId);
    void handleReconnect(const TargetId& targetId);
    void handleCrash(const TargetId& targetId);
    void handleNavigation(const TargetId& targetId, const PageUrl& newUrl);
    void handleTabClose(const TargetId& targetId);

    void attemptReconnect(const TargetId& targetId);
    void cancelReconnect(const TargetId& targetId);

    struct ReliabilityStats {
        size_t totalDisconnects = 0;
        size_t successfulReconnects = 0;
        size_t failedReconnects = 0;
        size_t crashes = 0;
        size_t tabsClosed = 0;
        uint32_t activeTargets = 0;
        uint32_t healthyTargets = 0;
    };

    ReliabilityStats getStats() const;
    void resetStats();

private:
    void transitionState(const TargetId& targetId, TargetState newState);
    void scheduleReconnect(const TargetId& targetId);
    void executeReconnect(const TargetId& targetId);
    void onReconnectComplete(const TargetId& targetId, bool success);
    std::chrono::milliseconds calculateBackoff(uint32_t attempts);

    std::atomic<bool> running_{false};
    std::jthread reconnectThread_;

    mutable std::mutex targetsMutex_;
    std::unordered_map<TargetId, std::shared_ptr<PageSession>, std::hash<TargetId>> sessions_;
    std::unordered_map<TargetId, TargetReliability, std::hash<TargetId>> reliability_;

    std::vector<TargetId> pendingReconnects_;
    std::mutex reconnectMutex_;
    std::condition_variable reconnectCV_;

    StateChangeCallback stateChangeCallback_;
    ErrorCallback errorCallback_;
    RecoveryCallback recoveryCallback_;

    std::atomic<size_t> totalDisconnects_{0};
    std::atomic<size_t> successfulReconnects_{0};
    std::atomic<size_t> failedReconnects_{0};
    std::atomic<size_t> crashes_{0};
    std::atomic<size_t> tabsClosed_{0};
};

} // namespace edgemon
