#pragma once

#include "core/Types.hpp"
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>

namespace edgemon {

class DOMObserver {
public:
    using ChangeCallback = std::function<void(const std::string& text, uint64_t hash)>;

    DOMObserver();
    ~DOMObserver();

    std::string generateObserverScript();
    std::string generateDebouncedObserverScript(int debounceMs = 300);

    static std::string extractTextFromNode(const std::string& selector = "body");
    static std::string extractVisibleText(const std::string& selector = "body");

    static uint64_t computeHash(const std::string& text);

private:
    static std::string baseObserverScript();
    static std::string debouncedObserverScript(int debounceMs);
};

class DOMChangeDetector {
public:
    using ChangeCallback = std::function<void(const std::string& text, bool isNew)>;

    explicit DOMChangeDetector();
    ~DOMChangeDetector();

    void setDebounceMs(int ms) { debounceMs_ = ms; }
    int getDebounceMs() const { return debounceMs_; }

    void setChangeCallback(ChangeCallback callback);
    void setHashCallback(DOMObserver::ChangeCallback callback);

    bool detect(const std::string& text);
    bool detect(const std::string& text, uint64_t hash);

    uint64_t getLastHash() const { return lastHash_; }
    bool hasChanged() const { return hasChanged_; }
    uint32_t getChangeCount() const { return changeCount_.load(); }

    void reset();
    void forceNextChange();

    struct DetectionStats {
        uint64_t lastHash = 0;
        uint32_t totalChanges = 0;
        uint32_t duplicatesSkipped = 0;
        uint32_t hashMatches = 0;
        double avgIntervalMs = 0;
    };

    DetectionStats getStats() const;

private:
    int debounceMs_ = 300;
    std::chrono::steady_clock::time_point lastChangeTime_;
    
    std::atomic<uint64_t> lastHash_{0};
    std::atomic<bool> hasChanged_{false};
    std::atomic<uint32_t> changeCount_{0};
    std::atomic<uint32_t> duplicatesSkipped_{0};
    std::atomic<uint32_t> hashMatches_{0};
    
    std::vector<uint64_t> recentHashes_;
    size_t maxRecentHashes_ = 10;
    
    ChangeCallback changeCallback_;
    DOMObserver::ChangeCallback hashCallback_;
};

} // namespace edgemon
