#include "extraction/OCRManager.hpp"
#include "extraction/OcrEngine.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cctype>
#include <regex>

namespace edgemon {

OCRManager::OCRManager(std::shared_ptr<ThreadPool> pool)
    : pool_(std::move(pool))
    , ocrPool_(std::make_shared<ThreadPool>(2))
    , initialized_(false)
    , running_(false)
    , jobQueue_(100) {
}

OCRManager::~OCRManager() {
    shutdown();
}

void OCRManager::initialize(const std::string& modelPath) {
    if (initialized_.exchange(true)) {
        LOG_WARN("OCRManager already initialized");
        return;
    }

    // State 1: worker initialized (queue + processing thread). This is NOT the
    // same as "OCR engine loaded" — engine availability is reported next.
    LOG_INFO("OCRManager initialized (worker thread + queue ready)");

    // State 2: load the real OCR engine (PP-OCRv3 via ONNX Runtime).
    // modelPath, when non-empty, overrides the default models/ directory.
    std::string modelsDir = modelPath.empty() ? "models" : modelPath;
    ocrEngine_ = std::make_shared<OcrEngine>();
    if (ocrEngine_->loadModels(modelsDir)) {
        LOG_INFO("OCRManager: OCR engine loaded (PP-OCRv3 via ONNX Runtime)");
    } else {
        LOG_ERROR("OCRManager: OCR engine unavailable: {}", ocrEngine_->lastError());
        LOG_WARN("OCRManager: recognition will return 'no text' until models are present; "
                 "place en_PP-OCRv3_det.onnx / en_PP-OCRv3_rec.onnx / en_dict.txt in {}",
                 modelsDir);
    }

    running_ = true;
    processingThread_ = std::jthread([this](std::stop_token token) {
        processQueue();
    });
}

void OCRManager::shutdown() {
    if (!running_.exchange(false)) {
        return;
    }

    jobQueue_.close();

    if (processingThread_.joinable()) {
        processingThread_.request_stop();
        processingThread_.join();
    }

    LOG_INFO("OCRManager shutdown complete");
}

void OCRManager::setConfig(const OCRConfig& config) {
    config_ = config;
    LOG_DEBUG("OCRManager config updated: confidence={}, lang={}", 
              config.confidenceThreshold, config.language);
}

OCRConfig OCRManager::getConfig() const {
    return config_;
}

void OCRManager::processFrame(const CapturedFrame& frame, ResultCallback callback) {
    if (!initialized_) {
        LOG_ERROR("OCRManager not initialized");
        OCRResult result;
        result.targetId = frame.targetId;
        result.rejected = true;
        result.rejectionReason = "OCR not initialized";
        callback(result);
        return;
    }

    OCRJob job;
    job.targetId = frame.targetId;
    job.frame = frame;
    job.queuedAt = std::chrono::steady_clock::now();
    job.callback = std::move(callback);

    if (jobQueue_.size() >= jobQueue_.capacity()) {
        LOG_WARN("OCRManager: job queue full, dropping frame for {}", frame.targetId.value);
        return;
    }
    jobQueue_.push(job);
}

void OCRManager::processFrame(const CapturedFrame& frame, const std::vector<RegionDiff>& regions,
                             ResultCallback callback) {
    if (!initialized_) {
        LOG_ERROR("OCRManager not initialized");
        OCRResult result;
        result.targetId = frame.targetId;
        result.rejected = true;
        result.rejectionReason = "OCR not initialized";
        callback(result);
        return;
    }

    OCRJob job;
    job.targetId = frame.targetId;
    job.frame = frame;
    job.regions = regions;
    job.queuedAt = std::chrono::steady_clock::now();
    job.callback = std::move(callback);

    if (jobQueue_.size() >= jobQueue_.capacity()) {
        LOG_WARN("OCRManager: job queue full, dropping frame for {}", frame.targetId.value);
        return;
    }
    jobQueue_.push(job);
}

void OCRManager::setResultCallback(ResultCallback callback) {
    resultCallback_ = std::move(callback);
}

void OCRManager::setErrorCallback(ErrorCallback callback) {
    errorCallback_ = std::move(callback);
}

size_t OCRManager::queueSize() const {
    return jobQueue_.size();
}

size_t OCRManager::processedCount() const {
    return totalProcessed_.load();
}

size_t OCRManager::rejectedCount() const {
    return totalRejected_.load();
}

OCRManager::Stats OCRManager::getStats() const {
    Stats stats;
    stats.totalProcessed = totalProcessed_.load();
    stats.totalAccepted = totalAccepted_.load();
    stats.totalRejected = totalRejected_.load();
    
    uint64_t processed = totalProcessed_.load();
    if (processed > 0) {
        stats.avgProcessingTimeMs = totalProcessingTime_.load() / processed;
        stats.avgConfidence = totalConfidence_.load() / processed;
    }
    
    return stats;
}

void OCRManager::resetStats() {
    totalProcessed_ = 0;
    totalAccepted_ = 0;
    totalRejected_ = 0;
    totalProcessingTime_ = 0;
    totalConfidence_ = 0;
}

void OCRManager::setMaxQueueSize(size_t maxSize) {
    LOG_DEBUG("OCRManager: max queue size set to {}", maxSize);
}

void OCRManager::setProcessingThreads(size_t count) {
    LOG_DEBUG("OCRManager: processing threads set to {}", count);
}

void OCRManager::processQueue() {
    while (running_) {
        auto job = jobQueue_.popFor(std::chrono::milliseconds(100));
        if (!job) continue;

        OCRJob ocrJob = *job;

        auto startTime = std::chrono::steady_clock::now();

        OCRResult result = performOCR(ocrJob.frame);

        result.targetId = ocrJob.targetId;
        result.cdpTargetId = ocrJob.frame.cdpTargetId.value;
        result.timestamp = ocrJob.queuedAt.time_since_epoch().count();

        if (meetsConfidenceThreshold(result)) {
            totalAccepted_++;
            result.rejected = false;
        } else {
            rejectResult(result, "Below confidence threshold");
        }

        auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        result.latencyMs = static_cast<uint64_t>(elapsed);

        totalProcessed_++;
        totalProcessingTime_ += elapsed;
        totalConfidence_ += result.confidence;

        LOG_DEBUG("OCR processed for {}: {} chars, conf={:.2f}, {}ms",
                  result.targetId.value, result.text.length(),
                  result.confidence, elapsed);

        if (resultCallback_) {
            try {
                resultCallback_(result);
            } catch (const std::exception& e) {
                LOG_ERROR("OCR result callback error: {}", e.what());
            }
        }

        if (ocrJob.callback) {
            try {
                ocrJob.callback(result);
            } catch (const std::exception& e) {
                LOG_ERROR("OCR job callback error: {}", e.what());
            }
        }
    }
}

OCRResult OCRManager::performOCR(const CapturedFrame& frame) {
    OCRResult result;
    result.targetId = frame.targetId;
    result.source = OCRResult::Source::Visual;
    result.timestamp = frame.timestamp;

    if (frame.pixels.empty()) {
        result.rejected = true;
        result.rejectionReason = "Empty frame";
        return result;
    }

    // State 3: OCR execution started.
    LOG_DEBUG("OCR execution started for {} ({} px)", frame.targetId.value,
              frame.pixels.size());

    // Engine availability gate: never pretend OCR succeeded without a real
    // engine. Missing models => "no text", not fabricated output.
    if (!ocrEngine_ || !ocrEngine_->available()) {
        result.rejected = true;
        result.rejectionReason = "OCR engine unavailable";
        LOG_WARN("OCR execution failed for {}: engine unavailable",
                 frame.targetId.value);
        return result;
    }

    // frame.pixels holds PNG bytes (from capture). Decode to BGRA via GDI+.
    std::vector<uint8_t> bgra;
    int imgW = 0, imgH = 0;
    if (!OcrEngine::decodePng(frame.pixels, bgra, imgW, imgH)) {
        result.rejected = true;
        result.rejectionReason = "PNG decode failed";
        LOG_WARN("OCR execution failed for {}: PNG decode failed",
                 frame.targetId.value);
        return result;
    }

    OcrPageResult page = ocrEngine_->recognize(bgra.data(), imgW, imgH);

    if (page.lines.empty()) {
        result.rejected = true;
        result.rejectionReason = "No text detected";
        result.confidence = 0.0f;
        LOG_DEBUG("OCR execution completed for {}: no text detected ({:.1f}ms)",
                  frame.targetId.value, ocrEngine_->lastLatencyMs());
        return result;
    }

    result.text = page.fullText();
    result.rawText = result.text;
    result.normalizedText = normalizeOCRText(result.text);
    result.confidence = page.meanConfidence;

    for (const auto& line : page.lines) {
        OCRResult::BoundingBox box;
        box.x = static_cast<float>(line.x);
        box.y = static_cast<float>(line.y);
        box.width = static_cast<float>(line.width);
        box.height = static_cast<float>(line.height);
        result.boxes.push_back(box);
    }

    LOG_DEBUG("OCR execution completed for {}: {} lines, {} chars, conf={:.2f} ({:.1f}ms)",
              frame.targetId.value, page.lines.size(), result.text.size(),
              result.confidence, ocrEngine_->lastLatencyMs());

    return result;
}

OCRResult OCRManager::performRegionOCR(const CapturedFrame& frame, const RegionDiff& region) {
    OCRResult result = performOCR(frame);

    if (!result.boxes.empty()) {
        result.boxes[0].x = static_cast<float>(region.x);
        result.boxes[0].y = static_cast<float>(region.y);
        result.boxes[0].width = static_cast<float>(region.width);
        result.boxes[0].height = static_cast<float>(region.height);
    }

    return result;
}

std::string OCRManager::normalizeOCRText(const std::string& raw) {
    if (raw.empty()) return "";

    std::string normalized = raw;

    normalized = std::regex_replace(normalized, std::regex("[\\t\\r]+"), " ");

    normalized = std::regex_replace(normalized, std::regex("\\s+"), " ");

    std::regex wordBoundary(R"(\s*([,;:!?.]+)\s*)");
    normalized = std::regex_replace(normalized, wordBoundary, "$1 ");

    while (!normalized.empty() && std::isspace(normalized.front())) {
        normalized.erase(normalized.begin());
    }
    while (!normalized.empty() && std::isspace(normalized.back())) {
        normalized.pop_back();
    }

    return normalized;
}

bool OCRManager::meetsConfidenceThreshold(const OCRResult& result) {
    return result.confidence >= config_.confidenceThreshold;
}

void OCRManager::rejectResult(OCRResult& result, const std::string& reason) {
    result.rejected = true;
    result.rejectionReason = reason;
    totalRejected_++;
    LOG_DEBUG("OCR rejected for {}: {} (conf={:.2f})",
              result.targetId.value, reason, result.confidence);
}

// ============================================================================
// OCRPipeline
// ============================================================================

OCRPipeline::OCRPipeline(std::shared_ptr<ThreadPool> pool)
    : pool_(std::move(pool))
    , ocrManager_(std::make_shared<OCRManager>(pool))
    , changeDetector_(std::make_shared<FrameChangeDetector>())
    , frameQueue_(50)
    , running_(false) {
}

OCRPipeline::~OCRPipeline() {
    stop();
}

void OCRPipeline::setOCRCallback(OCRManager::ResultCallback callback) {
    ocrManager_->setResultCallback(std::move(callback));
}

void OCRPipeline::setChangeDetectorConfig(const FrameChangeDetector::Config& config) {
    changeDetector_->setConfig(config);
}

void OCRPipeline::setOCRConfig(const OCRConfig& config) {
    ocrManager_->setConfig(config);
}

void OCRPipeline::addFrame(const CapturedFrame& frame) {
    if (!running_) return;

    if (frameQueue_.size() >= frameQueue_.capacity()) {
        LOG_WARN("OCRPipeline: frame queue full, dropping frame");
        return;
    }
    frameQueue_.push(frame);
}

void OCRPipeline::processAllPending() {
    while (running_) {
        auto frame = frameQueue_.popFor(std::chrono::milliseconds(10));
        if (!frame) break;

        CapturedFrame f = *frame;

        auto changeResult = changeDetector_->detect(f);

        if (!changeResult.hasChanged) {
            LOG_DEBUG("OCRPipeline: no change detected for {}", f.targetId.value);
            continue;
        }

        ocrManager_->processFrame(f, changeResult.regions, [](const OCRResult& result) {
            LOG_DEBUG("OCRPipeline OCR result: {} chars", result.text.length());
        });
    }
}

size_t OCRPipeline::pendingFrames() const {
    return frameQueue_.size();
}

size_t OCRPipeline::pendingOCRJobs() const {
    return ocrManager_->queueSize();
}

void OCRPipeline::start() {
    if (running_.exchange(true)) return;

    ocrManager_->initialize();
    changeDetector_->reset();

    processingThread_ = std::jthread([this](std::stop_token token) {
        while (!token.stop_requested()) {
            processAllPending();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    LOG_INFO("OCRPipeline started");
}

void OCRPipeline::stop() {
    if (!running_.exchange(false)) return;

    if (processingThread_.joinable()) {
        processingThread_.request_stop();
        processingThread_.join();
    }

    ocrManager_->shutdown();
    LOG_INFO("OCRPipeline stopped");
}

OCRManager::Stats OCRPipeline::getOCRStats() const {
    return ocrManager_->getStats();
}

FrameChangeDetector::Stats OCRPipeline::getChangeStats() const {
    return changeDetector_->getStats();
}

} // namespace edgemon
