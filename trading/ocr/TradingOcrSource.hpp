#pragma once

// trading/ocr/TradingOcrSource.hpp — drives the EXISTING EdgeMonitor OCR
// engine (PP-OCRv3 + CDP screenshot capture) as a trading data-ingestion
// source. Reuses edgemon's EdgeConnector/PageSession/OcrEngine/ChangeDetector;
// nothing in edgemon is modified. Emits validated OcrEvents onto the trading
// event bus. Degrades gracefully when Edge/CDP/OCR is unavailable.

#include "trading/core/event/TradingEventBus.hpp"
#include "trading/ocr/OcrTypes.hpp"
#include "trading/core/config/ConfigStore.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace edgemon {
class PageSession;
class ThreadPool;
}  // namespace edgemon

namespace trading {

// One monitored page.
struct OcrWatchTarget {
    std::string url;
    std::string role;    // price | news | chart | alert
    std::string label;
    double rateHz = 0.5;
    float minConfidence = 0.6f;
};

// Result of one OCR cycle over one target.
struct OcrCaptureResult {
    bool ok = false;
    std::string error;
    std::vector<OcrEvent> events;   // one per detected text region
};

class TradingOcrSource {
public:
    explicit TradingOcrSource(TradingEventBus& bus);
    ~TradingOcrSource();

    // Sets the pages to watch. Idempotent; safe to call before start().
    void configure(const std::vector<OcrWatchTarget>& targets);

    // Starts the monitoring thread(s). Returns false if nothing to watch.
    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    // Synchronous one-shot OCR of a single URL (used by [TEST] in the UI).
    // Reuses the real capture+recognize path. Returns false when no CDP/Edge.
    OcrCaptureResult ocrOnce(const OcrWatchTarget& target, int timeoutMs = 20000);

    // Stats.
    struct Stats {
        uint64_t cycles = 0;
        uint64_t events = 0;
        uint64_t deduped = 0;
        uint64_t lowConfidence = 0;
        uint64_t captureErrors = 0;
        double lastLatencyMs = 0.0;
    };
    Stats stats() const;

private:
    struct Watcher;  // per-target worker state

    void watcherLoop(Watcher& w);
    bool attachSession(Watcher& w);
    void emitEvent(Watcher& w, const OcrEvent& ev);

    TradingEventBus& bus_;
    std::vector<OcrWatchTarget> targets_;
    std::vector<std::unique_ptr<Watcher>> watchers_;
    std::atomic<bool> running_{false};
    std::thread controlThread_;   // manages watchers + reattach

    mutable std::mutex statsMutex_;
    Stats stats_;
};

} // namespace trading
