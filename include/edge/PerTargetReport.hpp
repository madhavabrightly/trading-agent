#pragma once

// PerTargetReport — one report directory per monitored target:
//
//   reports/<target-id>/
//     metadata.json      stable target identity + rule (no cookies/credentials)
//     events.jsonl       lifecycle events (CDP_AVAILABLE, TARGET_FOUND, ...)
//     final-report.md    human-readable aggregate at stop()
//     screenshots/       PNG only for CHANGED frames
//     ocr/<ts>.json      structured per-cycle OCR result
//     text/<ts>.txt      plain-text of new/changed OCR text
//
// Authentication safety: this writer never receives or persists cookies,
// credentials, or session tokens. It writes page pixels + OCR text only.

#include "core/Types.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <chrono>

namespace edgemon {

struct OcrCycleRecord {
    std::string timestamp;     // ISO-ish local "YYYY-MM-DD_HH-MM-SS"
    std::string targetId;
    std::string cdpTargetId;
    std::string url;
    std::string title;
    bool unchanged = false;    // SKIP_OCR_UNCHANGED cycle
    bool accepted = false;     // OCR ran and met confidence
    bool rejected = false;
    std::string rejectionReason;
    std::string text;
    uint64_t latencyMs = 0;
    float confidence = 0.0f;

    struct Line {
        std::string text;
        float confidence = 0.0f;
        float x = 0, y = 0, width = 0, height = 0;
    };
    std::vector<Line> lines;
};

struct ReportEvent {
    std::string timestamp;
    std::string event;         // e.g. CDP_AVAILABLE, TARGET_FOUND, TARGET_NOT_FOUND
    std::string detail;
};

class PerTargetReport {
public:
    PerTargetReport() = default;
    ~PerTargetReport();

    // Creates reports/<target-id>/ (and subdirs). Safe to call repeatedly.
    bool open(const TargetId& targetId, const std::string& cdpTargetId,
              const std::string& url, const std::string& title,
              TargetMatchRule rule, const std::string& profileHint);

    std::string rootPath() const { return rootPath_; }
    std::string targetId() const { return targetId_.value; }

    // metadata.json + events.jsonl
    void appendEvent(const std::string& event, const std::string& detail);

    // Writes one OCR cycle into ocr/<ts>.json; when !unchanged also writes the
    // screenshot PNG (screenshotPng may be empty) and text/<ts>.txt.
    // Returns the written ocr json path ("" on failure).
    std::string saveOcrCycle(const OcrCycleRecord& cycle,
                             const std::vector<uint8_t>* screenshotPng);

    // final-report.md aggregate. Call once at stop().
    void writeFinalReport(const std::string& extraSummary);

    size_t changedCycles() const { return changedCycles_.load(); }
    size_t unchangedCycles() const { return unchangedCycles_.load(); }
    size_t ocrAccepted() const { return ocrAccepted_.load(); }
    size_t ocrRejected() const { return ocrRejected_.load(); }

private:
    std::string safeDirName(const std::string& name);
    void ensureDir(const std::string& sub);

    TargetId targetId_;
    std::string rootPath_;
    std::string metadataUrl_;
    std::string metadataTitle_;
    std::string metadataCdp_;
    TargetMatchRule metadataRule_ = TargetMatchRule::Exact;
    std::string metadataProfile_;
    bool opened_ = false;

    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    std::atomic<size_t> changedCycles_{0};
    std::atomic<size_t> unchangedCycles_{0};
    std::atomic<size_t> ocrAccepted_{0};
    std::atomic<size_t> ocrRejected_{0};
};

// Shared helper so main.cpp / monitor can produce ISO-ish timestamps.
std::string timestampForFile(std::chrono::system_clock::time_point tp =
                                 std::chrono::system_clock::now());
std::string isoTimestamp(std::chrono::system_clock::time_point tp =
                             std::chrono::system_clock::now());

} // namespace edgemon
