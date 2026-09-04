#pragma once

#include "core/Types.hpp"
#include "core/TargetManager.hpp"
#include "threadpool.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <mutex>
#include <chrono>

namespace edgemon {

enum class TargetPriority {
    High,
    Normal,
    Low,
    Inactive
};

enum class CaptureMode {
    DOM_Only,
    Visual_Only,
    DOM_and_Visual,
    None
};

struct TargetPolicy {
    TargetId targetId;
    TargetPriority priority = TargetPriority::Normal;
    CaptureMode captureMode = CaptureMode::DOM_and_Visual;
    double domFPS = 1.0;
    double visualFPS = 1.0;
    bool ocrEnabled = true;
    bool embeddingEnabled = true;
    bool paused = false;
};

struct SchedulerConfig {
    size_t captureWorkers = 4;
    size_t ocrWorkers = 2;
    size_t embeddingWorkers = 2;
    size_t maxQueueSize = 500;
    bool adaptiveScheduling = true;
    double maxCPUPercent = 80.0;
};

class TargetScheduler : public std::enable_shared_from_this<TargetScheduler> {
public:
    using TargetCallback = std::function<void(const TargetId&, const std::string&)>;

    explicit TargetScheduler(SchedulerConfig config = {});
    ~TargetScheduler();

    void initialize();
    void shutdown();

    void registerTarget(const TargetId& targetId, const TargetPolicy& policy);
    void unregisterTarget(const TargetId& targetId);
    void updatePolicy(const TargetId& targetId, const TargetPolicy& policy);
    TargetPolicy getPolicy(const TargetId& targetId) const;

    void setTargetPriority(const TargetId& targetId, TargetPriority priority);
    void setCaptureMode(const TargetId& targetId, CaptureMode mode);
    void setFPS(const TargetId& targetId, double domFPS, double visualFPS);
    void pauseTarget(const TargetId& targetId);
    void resumeTarget(const TargetId& targetId);

    void pauseAll();
    void resumeAll();

    void setDOMCallback(std::function<void(const TargetId&, const std::string&)> callback);
    void setVisualCallback(std::function<void(const TargetId&, const CapturedFrame&)> callback);
    void setOCRCallback(std::function<void(const TargetId&, const OCRResult&)> callback);
    void setEmbeddingCallback(std::function<void(const TargetId&, const EmbeddingResult&)> callback);
    void setErrorCallback(TargetCallback callback);

    size_t activeTargets() const;
    size_t queueSize() const;

    struct SchedulerStats {
        size_t activeTargets = 0;
        size_t pausedTargets = 0;
        size_t captureQueueSize = 0;
        size_t ocrQueueSize = 0;
        size_t embeddingQueueSize = 0;
        size_t totalDOMEvents = 0;
        size_t totalVisualFrames = 0;
        size_t totalOCREvents = 0;
        size_t totalEmbeddings = 0;
        double cpuUsage = 0;
    };

    SchedulerStats getStats() const;
    void resetStats();

private:
    void scheduleLoop();
    void processTarget(const TargetId& targetId);
    void processDOM(const TargetId& targetId);
    void processVisual(const TargetId& targetId);
    void dispatchToOCR(const CapturedFrame& frame);
    void dispatchToEmbedding(const OCRResult& ocrResult);
    void dispatchToEmbedding(const std::string& text, const Observation& obs);
    void handleTargetError(const TargetId& targetId, const std::string& error);
    void adjustWorkerLoad();

    SchedulerConfig config_;
    std::shared_ptr<ThreadPool> capturePool_;
    std::shared_ptr<ThreadPool> ocrPool_;
    std::shared_ptr<ThreadPool> embeddingPool_;

    std::atomic<bool> running_{false};
    std::jthread schedulerThread_;

    mutable std::mutex policiesMutex_;
    std::unordered_map<TargetId, TargetPolicy, std::hash<TargetId>> policies_;
    std::unordered_map<TargetId, std::chrono::steady_clock::time_point, std::hash<TargetId>> lastDOMProcess_;
    std::unordered_map<TargetId, std::chrono::steady_clock::time_point, std::hash<TargetId>> lastVisualProcess_;

    BlockingQueue<std::pair<TargetId, std::string>> domQueue_;
    BlockingQueue<std::pair<TargetId, CapturedFrame>> visualQueue_;
    BlockingQueue<std::pair<TargetId, OCRResult>> ocrQueue_;
    BlockingQueue<std::pair<TargetId, EmbeddingResult>> embeddingQueue_;

    std::function<void(const TargetId&, const std::string&)> domCallback_;
    std::function<void(const TargetId&, const CapturedFrame&)> visualCallback_;
    std::function<void(const TargetId&, const OCRResult&)> ocrCallback_;
    std::function<void(const TargetId&, const EmbeddingResult&)> embeddingCallback_;
    TargetCallback errorCallback_;

    std::atomic<size_t> totalDOMEvents_{0};
    std::atomic<size_t> totalVisualFrames_{0};
    std::atomic<size_t> totalOCREvents_{0};
    std::atomic<size_t> totalEmbeddings_{0};
};

class TargetUI {
public:
    TargetUI();
    ~TargetUI();

    void initialize();
    void shutdown();

    void addTarget(const TargetPolicy& policy);
    void removeTarget(const TargetId& targetId);
    void updateTargetStatus(const TargetId& targetId, TargetState state);
    void updateTargetStats(const TargetId& targetId, const std::string& stat, double value);
    void updateTargetStats(const TargetId& targetId, const std::string& stat, const std::string& value);

    void setAddTargetCallback(std::function<void()> callback);
    void setLockTargetCallback(std::function<void(const TargetId&)> callback);
    void setUnlockTargetCallback(std::function<void(const TargetId&)> callback);
    void setPauseOCRCallback(std::function<void(const TargetId&)> callback);
    void setResumeOCRCallback(std::function<void(const TargetId&)> callback);
    void setRemoveTargetCallback(std::function<void(const TargetId&)> callback);

    struct TargetDisplay {
        TargetId targetId;
        std::string title;
        PageUrl url;
        TargetState state;
        std::string domStatus;
        std::string visualStatus;
        std::string ocrStatus;
        std::string lastUpdate;
        double ocrFPS;
        size_t embeddingCount;
        double cpuUsage;
        double gpuUsage;
        TargetPriority priority;
        CaptureMode captureMode;
    };

    std::vector<TargetDisplay> getAllTargets() const;
    TargetDisplay getTarget(const TargetId& targetId) const;
    void render();
    void renderTarget(const TargetDisplay& target);

private:
    std::vector<TargetDisplay> targets_;
    std::function<void()> addTargetCallback_;
    std::function<void(const TargetId&)> lockTargetCallback_;
    std::function<void(const TargetId&)> unlockTargetCallback_;
    std::function<void(const TargetId&)> pauseOCRCallback_;
    std::function<void(const TargetId&)> resumeOCRCallback_;
    std::function<void(const TargetId&)> removeTargetCallback_;
};

} // namespace edgemon
