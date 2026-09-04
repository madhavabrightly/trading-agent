#pragma once

#include "common.hpp"
#include "threadpool.hpp"
#include "capture/BrowserCapture.hpp"
#include "capture/ChangeDetector.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace edgemon {

class FrameBuffer;

class CaptureManager : public std::enable_shared_from_this<CaptureManager> {
public:
    explicit CaptureManager(std::shared_ptr<ThreadPool> pool);
    ~CaptureManager();

    void start();
    void stop();

    void registerTarget(const std::string& targetId);
    void unregisterTarget(const std::string& targetId);
    
    void setTargetFPS(const std::string& targetId, int fps);
    void enableVisualCapture(const std::string& targetId, bool enabled);

    bool captureFrame(const std::string& targetId, const CapturedFrame& frame);

    size_t pendingFrames() const;
    size_t activeTargets() const;

    void setFrameCallback(std::function<void(const CapturedFrame&)> callback);
    void setOCRCallback(std::function<void(const CapturedFrame&)> callback);

private:
    void processFrameQueue(std::stop_token token);
    void dispatchToOCR(const CapturedFrame& frame);

    std::shared_ptr<ThreadPool> pool_;
    std::shared_ptr<ThreadPool> ocrPool_;
    std::atomic<bool> running_;
    
    mutable std::mutex targetsMutex_;
    std::unordered_map<std::string, int> targetFPS_;
    std::unordered_map<std::string, bool> visualEnabled_;
    
    std::shared_ptr<FrameBuffer> frameBuffer_;
    std::shared_ptr<FrameChangeDetector> changeDetector_;
    
    std::jthread processingThread_;
    std::function<void(const CapturedFrame&)> frameCallback_;
    std::function<void(const CapturedFrame&)> ocrCallback_;
};

}
