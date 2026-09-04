#include "capture/CaptureManager.hpp"
#include "capture/FrameBuffer.hpp"
#include "capture/ChangeDetector.hpp"
#include "logger.hpp"

namespace edgemon {

CaptureManager::CaptureManager(std::shared_ptr<ThreadPool> pool)
    : pool_(std::move(pool))
    , ocrPool_(std::make_shared<ThreadPool>(2))
    , running_(false)
    , frameBuffer_(std::make_shared<FrameBuffer>(DEFAULT_FRAME_QUEUE_SIZE))
    , changeDetector_(std::make_shared<FrameChangeDetector>()) {
}

CaptureManager::~CaptureManager() {
    stop();
}

void CaptureManager::start() {
    if (running_.exchange(true)) return;
    
    processingThread_ = std::jthread([this](std::stop_token token) {
        processFrameQueue(std::move(token));
    });
    
    LOG_INFO("CaptureManager started");
}

void CaptureManager::stop() {
    if (!running_.exchange(false)) return;
    
    if (processingThread_.joinable()) {
        processingThread_.request_stop();
        processingThread_.join();
    }
    
    frameBuffer_->clear();
    LOG_INFO("CaptureManager stopped");
}

void CaptureManager::registerTarget(const std::string& targetId) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    targetFPS_[targetId] = 1;
    visualEnabled_[targetId] = false;
    LOG_DEBUG("Target registered for capture: {}", targetId);
}

void CaptureManager::unregisterTarget(const std::string& targetId) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    targetFPS_.erase(targetId);
    visualEnabled_.erase(targetId);
    LOG_DEBUG("Target unregistered from capture: {}", targetId);
}

void CaptureManager::setTargetFPS(const std::string& targetId, int fps) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    targetFPS_[targetId] = std::max(1, std::min(fps, 30));
}

void CaptureManager::enableVisualCapture(const std::string& targetId, bool enabled) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    visualEnabled_[targetId] = enabled;
    LOG_DEBUG("Visual capture {} for target {}", enabled ? "enabled" : "disabled", targetId);
}

bool CaptureManager::captureFrame(const std::string& targetId, const CapturedFrame& frame) {
    if (!running_) return false;
    std::lock_guard<std::mutex> lock(targetsMutex_);
    if (!running_) return false;
    return frameBuffer_->push(frame);
}

void CaptureManager::processFrameQueue(std::stop_token token) {
    while (running_ && !token.stop_requested()) {
        auto frame = frameBuffer_->popFor(std::chrono::milliseconds(100));
        if (!frame) continue;

        if (changeDetector_->hasFrameChanged(*frame)) {
            if (frameCallback_) {
                frameCallback_(*frame);
            }

            bool visualEnabled = false;
            {
                std::lock_guard<std::mutex> lock(targetsMutex_);
                auto it = visualEnabled_.find(frame->targetId.value);
                if (it != visualEnabled_.end()) {
                    visualEnabled = it->second;
                }
            }

            if (visualEnabled) {
                dispatchToOCR(*frame);
            }
        }
    }
}

void CaptureManager::dispatchToOCR(const CapturedFrame& frame) {
    ocrPool_->submit([this, frame]() {
        if (ocrCallback_) {
            ocrCallback_(frame);
        }
    });
}

size_t CaptureManager::pendingFrames() const {
    return frameBuffer_->size();
}
size_t CaptureManager::activeTargets() const {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    return targetFPS_.size();
}

void CaptureManager::setFrameCallback(std::function<void(const CapturedFrame&)> callback) {
    frameCallback_ = std::move(callback);
}

void CaptureManager::setOCRCallback(std::function<void(const CapturedFrame&)> callback) {
    ocrCallback_ = std::move(callback);
}

}
