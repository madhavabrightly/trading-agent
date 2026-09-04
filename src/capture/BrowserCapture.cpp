#include "capture/BrowserCapture.hpp"
#include "logger.hpp"
#include <algorithm>

namespace edgemon {

static std::atomic<size_t> totalCaptures_{0};
static std::atomic<size_t> successfulCaptures_{0};
static std::atomic<size_t> failedCaptures_{0};
static std::atomic<size_t> unavailableCaptures_{0};
static std::atomic<size_t> backgroundSkipped_{0};

BrowserCapture::BrowserCapture(std::shared_ptr<PageSession> session)
    : session_(std::move(session))
    , lastStatus_(CaptureStatus::Error) {
}

BrowserCapture::~BrowserCapture() = default;

CapturedFrame BrowserCapture::capture() {
    return capture(defaultConfig_);
}

CapturedFrame BrowserCapture::capture(const CaptureConfig& config) {
    CapturedFrame frame;
    frame.targetId = targetId_;
    frame.cdpTargetId = cdpTargetId_;
    frame.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    if (!session_ || !session_->isConnected()) {
        frame.status = CaptureStatus::NotConnected;
        frame.statusMessage = "Page session not connected";
        lastStatus_ = CaptureStatus::NotConnected;
        lastError_ = frame.statusMessage;
        return frame;
    }

    if (!isPageRendering()) {
        frame.isBackground = true;
        frame.status = CaptureStatus::RenderUnavailable;
        frame.statusMessage = "Page not rendering (minimized/background)";
        lastStatus_ = CaptureStatus::RenderUnavailable;
        lastError_ = frame.statusMessage;
        return frame;
    }

    try {
        nlohmann::json result = session_->captureScreenshot(
            config.format,
            config.quality,
            config.fullPage
        );

        if (result.empty() || !result.contains("data")) {
            frame.status = CaptureStatus::CaptureUnavailable;
            frame.statusMessage = "No screenshot data returned";
            lastStatus_ = CaptureStatus::CaptureUnavailable;
            return frame;
        }

        std::string base64Data = result["data"].get<std::string>();
        frame.pixels = decodeBase64Image(base64Data);

        if (frame.pixels.empty()) {
            frame.status = CaptureStatus::CaptureUnavailable;
            frame.statusMessage = "Failed to decode screenshot";
            lastStatus_ = CaptureStatus::CaptureUnavailable;
            return frame;
        }

        frame.width = result.value("width", 0);
        frame.height = result.value("height", 0);
        frame.format = PixelFormat::RGBA;
        frame.status = CaptureStatus::Success;
        frame.isStale = false;
        lastStatus_ = CaptureStatus::Success;

        LOG_DEBUG("BrowserCapture: captured {}x{} for {}",
                  frame.width, frame.height, targetId_.value);

    } catch (const std::exception& e) {
        frame.status = CaptureStatus::Error;
        frame.statusMessage = e.what();
        lastStatus_ = CaptureStatus::Error;
        lastError_ = e.what();
    }

    return frame;
}

void BrowserCapture::captureAsync(CaptureCallback callback) {
    captureAsync(defaultConfig_, std::move(callback));
}

void BrowserCapture::captureAsync(const CaptureConfig& config, CaptureCallback callback) {
    if (!session_) {
        CapturedFrame frame;
        frame.targetId = targetId_;
        frame.status = CaptureStatus::NotConnected;
        callback(frame);
        return;
    }

    std::thread([this, config, callback = std::move(callback)]() mutable {
        auto frame = capture(config);
        callback(frame);
    }).detach();
}

bool BrowserCapture::isPageConnected() const {
    return session_ && session_->isConnected();
}

bool BrowserCapture::isPageVisible() const {
    if (!session_) return false;

    auto result = session_->executeJavaScriptSync(
        R"(
            (function() {
                const rect = document.body ? document.body.getBoundingClientRect() : null;
                return rect ? (rect.width > 0 && rect.height > 0) : true;
            })()
        )",
        std::chrono::milliseconds(500)
    );

    if (!result) return true;
    if (result->contains("result") && result->at("result").contains("value")) {
        return result->at("result").at("value").get<bool>();
    }
    return true;
}

bool BrowserCapture::isPageRendering() const {
    if (!session_) return false;

    auto result = session_->executeJavaScriptSync(
        R"(
            (function() {
                if (document.hidden !== undefined && document.hidden) return false;
                if (document.visibilityState === 'hidden') return false;
                return true;
            })()
        )",
        std::chrono::milliseconds(500)
    );

    if (!result) return true;
    if (result->contains("result") && result->at("result").contains("value")) {
        return result->at("result").at("value").get<bool>();
    }
    return true;
}

std::vector<uint8_t> BrowserCapture::decodeBase64Image(const std::string& base64Data) {
    std::vector<uint8_t> decoded;

    if (base64Data.empty()) {
        return decoded;
    }

    static const char* base64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    size_t padding = 0;
    if (!base64Data.empty() && base64Data.back() == '=') padding++;
    if (base64Data.size() >= 2 && base64Data[base64Data.size() - 2] == '=') padding++;

    decoded.reserve(base64Data.size() * 3 / 4);

    std::array<int, 256> lookup{};
    lookup.fill(-1);
    for (int i = 0; i < 64; i++) {
        lookup[static_cast<unsigned char>(base64Chars[i])] = i;
    }

    std::array<uint8_t, 4> buffer{};
    size_t bufferPos = 0;

    for (size_t i = 0; i < base64Data.size(); i++) {
        char c = base64Data[i];
        if (c == '=') break;

        int val = lookup[static_cast<unsigned char>(c)];
        if (val < 0) continue;

        buffer[bufferPos++] = static_cast<uint8_t>(val);

        if (bufferPos == 4) {
            decoded.push_back((buffer[0] << 2) | (buffer[1] >> 4));
            decoded.push_back((buffer[1] << 4) | (buffer[2] >> 2));
            decoded.push_back((buffer[2] << 6) | buffer[3]);
            bufferPos = 0;
        }
    }

    if (bufferPos > 0) {
        for (size_t i = bufferPos; i < 4; i++) {
            buffer[i] = 0;
        }
        decoded.push_back((buffer[0] << 2) | (buffer[1] >> 4));
        if (bufferPos > 1) {
            decoded.push_back((buffer[1] << 4) | (buffer[2] >> 2));
        }
    }

    decoded.resize(decoded.size() - padding);

    return decoded;
}

CapturedFrame BrowserCapture::createFrame(const std::string& base64Data, uint32_t width, uint32_t height) {
    CapturedFrame frame;
    frame.targetId = targetId_;
    frame.cdpTargetId = cdpTargetId_;
    frame.width = width;
    frame.height = height;
    frame.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
    frame.pixels = decodeBase64Image(base64Data);
    frame.format = PixelFormat::RGBA;
    frame.status = CaptureStatus::Success;
    return frame;
}

// ============================================================================
// VisualCaptureManager
// ============================================================================

VisualCaptureManager::VisualCaptureManager(std::shared_ptr<ThreadPool> pool)
    : pool_(std::move(pool))
    , frameQueue_(100) {
}

VisualCaptureManager::~VisualCaptureManager() {
    stopAllCaptures();
}

void VisualCaptureManager::registerTarget(const TargetId& targetId, std::shared_ptr<PageSession> session) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    
    if (targets_.contains(targetId)) {
        LOG_WARN("VisualCaptureManager: target {} already registered", targetId.value);
        return;
    }
    
    TargetCapture tc;
    tc.session = session;
    tc.capture = std::make_shared<BrowserCapture>(session);
    tc.capture->setTargetId(targetId);
    tc.fps = 1.0;
    
    // TargetCapture holds a std::atomic (non-movable), so construct in place.
    auto [it, inserted] = targets_.emplace(targetId, std::move(tc));
    if (!inserted) {
        LOG_WARN("VisualCaptureManager: target {} already registered", targetId.value);
        return;
    }
    
    LOG_INFO("VisualCaptureManager: registered target {}", targetId.value);
}

void VisualCaptureManager::unregisterTarget(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    
    auto it = targets_.find(targetId);
    if (it != targets_.end()) {
        it->second.capturing = false;
        if (it->second.captureThread.joinable()) {
            it->second.captureThread.request_stop();
            it->second.captureThread.join();
        }
        targets_.erase(it);
        LOG_INFO("VisualCaptureManager: unregistered target {}", targetId.value);
    }
}

void VisualCaptureManager::setTargetFPS(const TargetId& targetId, double fps) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    
    auto it = targets_.find(targetId);
    if (it != targets_.end()) {
        it->second.fps = std::max(0.1, std::min(fps, 30.0));
        LOG_DEBUG("VisualCaptureManager: target {} FPS set to {}", targetId.value, it->second.fps);
    }
}

void VisualCaptureManager::setTargetConfig(const TargetId& targetId, const CaptureConfig& config) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    
    auto it = targets_.find(targetId);
    if (it != targets_.end()) {
        it->second.config = config;
        LOG_DEBUG("VisualCaptureManager: updated config for target {}", targetId.value);
    }
}

void VisualCaptureManager::startCapture(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    
    auto it = targets_.find(targetId);
    if (it == targets_.end()) {
        LOG_ERROR("VisualCaptureManager: target {} not registered", targetId.value);
        return;
    }
    
    if (it->second.capturing.exchange(true)) {
        LOG_DEBUG("VisualCaptureManager: target {} already capturing", targetId.value);
        return;
    }
    
    it->second.captureThread = std::jthread([this, targetId](std::stop_token token) {
        TargetId capturedTargetId = targetId;
        std::shared_ptr<BrowserCapture> capturedCapture;
        {
            std::lock_guard<std::mutex> lock(targetsMutex_);
            auto it = targets_.find(targetId);
            if (it != targets_.end()) {
                capturedCapture = it->second.capture;
            }
        }
        if (capturedCapture) {
            captureLoop(capturedTargetId, capturedCapture);
        }
    });
    
    LOG_INFO("VisualCaptureManager: started capture for target {}", targetId.value);
}

void VisualCaptureManager::stopCapture(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    
    auto it = targets_.find(targetId);
    if (it != targets_.end() && it->second.capturing.exchange(false)) {
        if (it->second.captureThread.joinable()) {
            it->second.captureThread.request_stop();
            it->second.captureThread.join();
        }
        LOG_INFO("VisualCaptureManager: stopped capture for target {}", targetId.value);
    }
}

void VisualCaptureManager::startAllCaptures() {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    
    for (auto& [targetId, tc] : targets_) {
        if (!tc.capturing) {
            tc.capturing = true;
            TargetId capturedTargetId = targetId;
            tc.captureThread = std::jthread([this, capturedTargetId](std::stop_token token) {
                std::shared_ptr<BrowserCapture> capturedCapture;
                {
                    std::lock_guard<std::mutex> lock(targetsMutex_);
                    auto it = targets_.find(capturedTargetId);
                    if (it != targets_.end()) {
                        capturedCapture = it->second.capture;
                    }
                }
                if (capturedCapture) {
                    captureLoop(capturedTargetId, capturedCapture);
                }
            });
        }
    }
    
    LOG_INFO("VisualCaptureManager: started all captures ({} targets)", targets_.size());
}

void VisualCaptureManager::stopAllCaptures() {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    
    for (auto& [targetId, tc] : targets_) {
        tc.capturing = false;
        if (tc.captureThread.joinable()) {
            tc.captureThread.request_stop();
            tc.captureThread.join();
        }
    }
    
    frameQueue_.clear();
    
    LOG_INFO("VisualCaptureManager: stopped all captures");
}

void VisualCaptureManager::setFrameCallback(FrameCallback callback) {
    frameCallback_ = std::move(callback);
}

size_t VisualCaptureManager::pendingFrames() const {
    return frameQueue_.size();
}

size_t VisualCaptureManager::activeTargets() const {
    std::lock_guard<std::mutex> lock(targetsMutex_);
    return targets_.size();
}

VisualCaptureManager::CaptureStats VisualCaptureManager::getStats() const {
    CaptureStats stats;
    stats.totalCaptures = totalCaptures_.load();
    stats.successful = successfulCaptures_.load();
    stats.failed = failedCaptures_.load();
    stats.unavailable = unavailableCaptures_.load();
    stats.backgroundSkipped = backgroundSkipped_.load();
    return stats;
}

void VisualCaptureManager::resetStats() {
    totalCaptures_ = 0;
    successfulCaptures_ = 0;
    failedCaptures_ = 0;
    unavailableCaptures_ = 0;
    backgroundSkipped_ = 0;
}

void VisualCaptureManager::captureLoop(const TargetId& targetId, std::shared_ptr<BrowserCapture> capture) {
    std::chrono::milliseconds interval(1000);
    
    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        auto it = targets_.find(targetId);
        if (it != targets_.end()) {
            interval = std::chrono::milliseconds(static_cast<int>(1000.0 / it->second.fps));
        }
    }
    
    while (true) {
        auto sleepStart = std::chrono::steady_clock::now();
        
        {
            std::lock_guard<std::mutex> lock(targetsMutex_);
            auto it = targets_.find(targetId);
            if (it == targets_.end() || !it->second.capturing) {
                break;
            }
            interval = std::chrono::milliseconds(static_cast<int>(1000.0 / it->second.fps));
        }
        
        CapturedFrame frame = capture->capture();
        
        if (frame.status == CaptureStatus::Success) {
            totalCaptures_++;
            successfulCaptures_++;
            
            // Bounded queue: drop the frame when full instead of blocking the
            // capture loop (backpressure by dropping is correct for visuals).
            if (frameQueue_.size() >= frameQueue_.capacity()) {
                LOG_WARN("VisualCaptureManager: frame queue full, dropping frame");
            } else {
                frameQueue_.push(frame);
            }
            
            if (frameCallback_) {
                frameCallback_(frame);
            }
        } else if (frame.status == CaptureStatus::RenderUnavailable) {
            backgroundSkipped_++;
            LOG_DEBUG("VisualCaptureManager: skipped capture for background target {}", targetId.value);
        } else if (frame.status == CaptureStatus::NotConnected) {
            failedCaptures_++;
            LOG_WARN("VisualCaptureManager: target {} not connected", targetId.value);
            break;
        } else {
            failedCaptures_++;
        }
        
        auto elapsed = std::chrono::steady_clock::now() - sleepStart;
        if (elapsed < interval) {
            std::this_thread::sleep_for(interval - elapsed);
        }
    }
    
    LOG_DEBUG("VisualCaptureManager: capture loop ended for target {}", targetId.value);
}

CapturedFrame VisualCaptureManager::popFrame() {
    auto frame = frameQueue_.popFor(std::chrono::milliseconds(100));
    if (frame) {
        return *frame;
    }
    
    CapturedFrame empty;
    empty.status = CaptureStatus::Timeout;
    return empty;
}

} // namespace edgemon
