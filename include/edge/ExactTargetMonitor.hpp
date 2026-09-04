#pragma once

// ExactTargetMonitor — continuous OCR of ONE exact CDP page target.
//
// Pipeline (strict):
//   Selected Target -> CDP Page -> Page.captureScreenshot -> PNG decode
//   -> change detection -> (unchanged: SKIP_OCR_UNCHANGED) -> real PP-OCRv3
//   -> structured result -> PerTargetReport under reports/<target-id>/.
//
// The monitor is locked to the resolved target; it NEVER silently attaches to
// another tab, never creates pages, and never touches cookies.

#include "core/Types.hpp"
#include "edge/PageSession.hpp"
#include "edge/PerTargetReport.hpp"
#include "capture/ChangeDetector.hpp"
#include "threadpool.hpp"
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <functional>
#include <mutex>

namespace edgemon {

class ExactTargetMonitor : public std::enable_shared_from_this<ExactTargetMonitor> {
public:
    struct TargetInfo {
        TargetId internalId;
        CDPTargetId cdpTargetId;
        std::string webSocketUrl;
        PageUrl url;
        std::string title;
        TargetMatchRule rule = TargetMatchRule::Exact;
        std::string profileHint;
    };

    using LogCallback = std::function<void(const std::string& line)>;
    // Called ONLY after a real screenshot + real PP-OCRv3 result was accepted
    // and successfully saved to the per-target report.
    using SavedOcrCallback = std::function<void(const std::string& cdpTargetId,
                                                const std::string& url,
                                                const std::string& title,
                                                const std::string& text,
                                                float confidence)>;

    ExactTargetMonitor(std::shared_ptr<ThreadPool> pool,
                       TargetInfo info,
                       std::shared_ptr<PerTargetReport> report);
    ~ExactTargetMonitor();

    void setLogCallback(LogCallback cb) { logCb_ = std::move(cb); }
    void setSavedOcrCallback(SavedOcrCallback cb) { savedOcrCb_ = std::move(cb); }

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    void setInterval(std::chrono::milliseconds interval) { interval_ = interval; }
    std::chrono::milliseconds interval() const { return interval_; }

    // Current (live) target identity — read by the GUI status field.
    std::string currentCdpTargetId() const;
    std::string currentUrl() const;
    std::string currentTitle() const;
    // The durable URL RULE (never changes on navigation; used for re-resolve).
    std::string currentRuleUrl() const;

    // Refresh title/url each cycle via Runtime.evaluate (used in reports).
    void refreshPageMetadata();

    // Recovery: re-resolve the target URL rule against a fresh /json/list.
    // Returns the new CDP target id when reattached, empty otherwise.
    std::string reattachAfterRestart();

    size_t changedCycles() const { return changedCycles_.load(); }
    size_t unchangedCycles() const { return unchangedCycles_.load(); }

    // Report writer access (for CLI status messages).
    std::shared_ptr<PerTargetReport> report() const { return report_; }

private:
    void loop(std::stop_token token);
    void pushLog(const std::string& line);
    bool attach();
    void detach();

    std::shared_ptr<ThreadPool> pool_;
    TargetInfo info_;
    std::shared_ptr<PerTargetReport> report_;
    std::shared_ptr<PageSession> session_;
    std::unique_ptr<FrameChangeDetector> changeDetector_;

    // The durable URL rule (set once at construction). info_.url is the LIVE
    // page URL used for display/metadata; reattach always matches against
    // ruleUrl_ so an in-page navigation cannot silently move the monitor.
    std::string ruleUrl_;

    // Guards the live identity (info_.cdpTargetId/url/title) read by the GUI
    // while the monitor thread refreshes it.
    mutable std::mutex infoMutex_;
    std::atomic<size_t> acceptedSaves_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> sessionLive_{false};
    std::atomic<size_t> changedCycles_{0};
    std::atomic<size_t> unchangedCycles_{0};
    std::atomic<size_t> cycle_{0};

    std::chrono::milliseconds interval_{1000};
    std::jthread thread_;
    LogCallback logCb_;
    SavedOcrCallback savedOcrCb_;
};

} // namespace edgemon
