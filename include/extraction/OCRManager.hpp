#pragma once

#include "core/Types.hpp"
#include "capture/BrowserCapture.hpp"
#include "capture/ChangeDetector.hpp"
#include "extraction/OcrEngine.hpp"
#include "threadpool.hpp"
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>

namespace edgemon {

struct OCRResult {
    TargetId targetId;
    std::string cdpTargetId;   // CDP target the frame was captured from
    std::string url;           // page URL at capture time (filled by caller)
    std::string title;         // page title at capture time (filled by caller)
    std::string text;
    float confidence = 0.0f;
    uint64_t latencyMs = 0;    // queue + inference wall time for this cycle
    
    struct BoundingBox {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };
    
    std::vector<BoundingBox> boxes;
    uint64_t timestamp = 0;
    
    bool rejected = false;
    std::string rejectionReason;
    std::string rawText;
    std::string normalizedText;
    
    enum class Source {
        Visual,
        DOM,
        Unknown
    } source = Source::Visual;
};

struct OCRJob {
    TargetId targetId;
    CapturedFrame frame;
    std::vector<RegionDiff> regions;
    std::chrono::steady_clock::time_point queuedAt;
    std::function<void(const OCRResult&)> callback;
};

struct OCRConfig {
    float confidenceThreshold = 0.7f;
    std::string language = "en";
    bool enableCorrection = true;
    bool preserveLayout = true;
    int maxTextLength = 10000;
};

class OCRManager : public std::enable_shared_from_this<OCRManager> {
public:
    using ResultCallback = std::function<void(const OCRResult& result)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    explicit OCRManager(std::shared_ptr<ThreadPool> pool);
    ~OCRManager();

    void initialize(const std::string& modelPath = "");
    void shutdown();
    bool isInitialized() const { return initialized_.load(); }

    void setConfig(const OCRConfig& config);
    OCRConfig getConfig() const;

    void processFrame(const CapturedFrame& frame, ResultCallback callback);
    void processFrame(const CapturedFrame& frame, const std::vector<RegionDiff>& regions, ResultCallback callback);

    void setResultCallback(ResultCallback callback);
    void setErrorCallback(ErrorCallback callback);

    size_t queueSize() const;
    size_t processedCount() const;
    size_t rejectedCount() const;

    struct Stats {
        uint64_t totalProcessed = 0;
        uint64_t totalAccepted = 0;
        uint64_t totalRejected = 0;
        double avgProcessingTimeMs = 0;
        double avgConfidence = 0;
    };
    Stats getStats() const;
    void resetStats();

    void setMaxQueueSize(size_t maxSize);
    void setProcessingThreads(size_t count);

    // True when the real OCR engine (PP-OCRv3 via ONNX Runtime) finished
    // loading. Until then, processFrame rejects frames as "engine unavailable".
    bool engineAvailable() const {
        return ocrEngine_ && ocrEngine_->available();
    }

private:
    void processQueue();
    OCRResult performOCR(const CapturedFrame& frame);
    OCRResult performRegionOCR(const CapturedFrame& frame, const RegionDiff& region);
    std::string normalizeOCRText(const std::string& raw);
    bool meetsConfidenceThreshold(const OCRResult& result);
    void rejectResult(OCRResult& result, const std::string& reason);

    std::shared_ptr<ThreadPool> pool_;
    std::shared_ptr<ThreadPool> ocrPool_;

    std::shared_ptr<OcrEngine> ocrEngine_;

    std::atomic<bool> initialized_;
    std::atomic<bool> running_;
    
    BlockingQueue<OCRJob> jobQueue_;
    std::jthread processingThread_;
    
    OCRConfig config_;
    
    ResultCallback resultCallback_;
    ErrorCallback errorCallback_;
    
    std::atomic<uint64_t> totalProcessed_{0};
    std::atomic<uint64_t> totalAccepted_{0};
    std::atomic<uint64_t> totalRejected_{0};
    std::atomic<double> totalProcessingTime_{0};
    std::atomic<double> totalConfidence_{0};
};

class OCRPipeline {
public:
    explicit OCRPipeline(std::shared_ptr<ThreadPool> pool);
    ~OCRPipeline();

    void setOCRCallback(OCRManager::ResultCallback callback);
    void setChangeDetectorConfig(const FrameChangeDetector::Config& config);
    void setOCRConfig(const OCRConfig& config);

    void addFrame(const CapturedFrame& frame);
    void processAllPending();

    size_t pendingFrames() const;
    size_t pendingOCRJobs() const;

    void start();
    void stop();

    OCRManager::Stats getOCRStats() const;
    FrameChangeDetector::Stats getChangeStats() const;

private:
    std::shared_ptr<ThreadPool> pool_;
    std::shared_ptr<OCRManager> ocrManager_;
    std::shared_ptr<FrameChangeDetector> changeDetector_;
    
    BlockingQueue<CapturedFrame> frameQueue_;
    std::atomic<bool> running_;
    std::jthread processingThread_;
};

} // namespace edgemon
