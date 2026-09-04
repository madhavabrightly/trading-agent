#pragma once

#include "core/Types.hpp"
#include "edge/PageSession.hpp"
#include "threadpool.hpp"
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>

namespace edgemon {

enum class CaptureStatus {
    Success,
    NotConnected,
    CaptureUnavailable,
    RenderUnavailable,
    Timeout,
    Error
};

struct CapturedFrame {
    TargetId targetId;
    CDPTargetId cdpTargetId;
    uint64_t timestamp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::Unknown;
    std::vector<uint8_t> pixels;
    CaptureStatus status = CaptureStatus::Error;
    std::string statusMessage;
    bool isStale = false;
    bool isBackground = false;
};

struct CaptureConfig {
    std::string format = "png";
    int quality = 90;
    bool fullPage = false;
    bool clipToViewport = true;
    uint32_t maxWidth = 3840;
    uint32_t maxHeight = 2160;
};

class BrowserCapture {
public:
    using CaptureCallback = std::function<void(const CapturedFrame& frame)>;

    explicit BrowserCapture(std::shared_ptr<PageSession> session);
    ~BrowserCapture();

    CapturedFrame capture();
    CapturedFrame capture(const CaptureConfig& config);
    
    void captureAsync(CaptureCallback callback);
    void captureAsync(const CaptureConfig& config, CaptureCallback callback);

    void setTargetId(TargetId id) { targetId_ = std::move(id); }
    TargetId getTargetId() const { return targetId_; }

    bool isPageConnected() const;
    bool isPageVisible() const;
    bool isPageRendering() const;

    CaptureStatus getLastStatus() const { return lastStatus_; }
    std::string getLastError() const { return lastError_; }

private:
    std::vector<uint8_t> decodeBase64Image(const std::string& base64Data);
    CapturedFrame createFrame(const std::string& base64Data, uint32_t width, uint32_t height);

    std::shared_ptr<PageSession> session_;
    TargetId targetId_;
    CDPTargetId cdpTargetId_;
    
    std::atomic<CaptureStatus> lastStatus_;
    std::string lastError_;
    
    CaptureConfig defaultConfig_;
};

class VisualCaptureManager : public std::enable_shared_from_this<VisualCaptureManager> {
public:
    using FrameCallback = std::function<void(const CapturedFrame& frame)>;

    explicit VisualCaptureManager(std::shared_ptr<ThreadPool> pool);
    ~VisualCaptureManager();

    void registerTarget(const TargetId& targetId, std::shared_ptr<PageSession> session);
    void unregisterTarget(const TargetId& targetId);
    
    void setTargetFPS(const TargetId& targetId, double fps);
    void setTargetConfig(const TargetId& targetId, const CaptureConfig& config);
    
    void startCapture(const TargetId& targetId);
    void stopCapture(const TargetId& targetId);
    void startAllCaptures();
    void stopAllCaptures();

    void setFrameCallback(FrameCallback callback);

    size_t pendingFrames() const;
    size_t activeTargets() const;

    struct CaptureStats {
        size_t totalCaptures = 0;
        size_t successful = 0;
        size_t failed = 0;
        size_t unavailable = 0;
        size_t backgroundSkipped = 0;
    };
    
    CaptureStats getStats() const;
    void resetStats();

private:
    void captureLoop(const TargetId& targetId, std::shared_ptr<BrowserCapture> capture);
    CapturedFrame popFrame();

    struct TargetCapture {
        TargetCapture() = default;
        TargetCapture(TargetCapture&& other) noexcept
            : session(std::move(other.session))
            , capture(std::move(other.capture))
            , fps(other.fps)
            , config(std::move(other.config))
            , captureThread(std::move(other.captureThread)) {
            // capturing is a std::atomic: leave it default (false); the
            // capturing flag is re-set explicitly by the caller when needed.
            other.capturing = false;
        }
        TargetCapture& operator=(TargetCapture&& other) noexcept {
            if (this != &other) {
                session = std::move(other.session);
                capture = std::move(other.capture);
                fps = other.fps;
                config = std::move(other.config);
                captureThread = std::move(other.captureThread);
                other.capturing = false;
            }
            return *this;
        }
        TargetCapture(const TargetCapture&) = delete;
        TargetCapture& operator=(const TargetCapture&) = delete;

        std::shared_ptr<PageSession> session;
        std::shared_ptr<BrowserCapture> capture;
        double fps = 1.0;
        CaptureConfig config;
        std::atomic<bool> capturing{false};
        std::jthread captureThread;
    };

    std::shared_ptr<ThreadPool> pool_;
    BlockingQueue<CapturedFrame> frameQueue_;
    
    mutable std::mutex targetsMutex_;
    std::unordered_map<TargetId, TargetCapture, std::hash<TargetId>> targets_;
    
    FrameCallback frameCallback_;
    
    std::atomic<size_t> totalCaptures_{0};
    std::atomic<size_t> successfulCaptures_{0};
    std::atomic<size_t> failedCaptures_{0};
    std::atomic<size_t> unavailableCaptures_{0};
    std::atomic<size_t> backgroundSkipped_{0};
};

} // namespace edgemon
