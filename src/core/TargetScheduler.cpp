#include "core/TargetScheduler.hpp"
#include "extraction/DOMObserver.hpp"
#include "extraction/DOMExtractor.hpp"
#include "capture/BrowserCapture.hpp"
#include "embedding/EmbeddingEngine.hpp"
#include "logger.hpp"
#include <algorithm>

namespace edgemon {

TargetScheduler::TargetScheduler(SchedulerConfig config)
    : config_(config)
    , domQueue_(config.maxQueueSize)
    , visualQueue_(config.maxQueueSize)
    , ocrQueue_(config.maxQueueSize)
    , embeddingQueue_(config.maxQueueSize) {
}

TargetScheduler::~TargetScheduler() {
    shutdown();
}

void TargetScheduler::initialize() {
    if (running_.exchange(true)) {
        return;
    }

    capturePool_ = std::make_shared<ThreadPool>(config_.captureWorkers);
    ocrPool_ = std::make_shared<ThreadPool>(config_.ocrWorkers);
    embeddingPool_ = std::make_shared<ThreadPool>(config_.embeddingWorkers);

    schedulerThread_ = std::jthread([this](std::stop_token token) {
        scheduleLoop();
    });

    LOG_INFO("TargetScheduler: initialized with {} capture, {} OCR, {} embedding workers",
             config_.captureWorkers, config_.ocrWorkers, config_.embeddingWorkers);
}

void TargetScheduler::shutdown() {
    if (!running_.exchange(false)) {
        return;
    }

    if (schedulerThread_.joinable()) {
        schedulerThread_.request_stop();
        schedulerThread_.join();
    }

    capturePool_->wait();
    ocrPool_->wait();
    embeddingPool_->wait();

    domQueue_.clear();
    visualQueue_.clear();
    ocrQueue_.clear();
    embeddingQueue_.clear();

    LOG_INFO("TargetScheduler: shutdown complete");
}

void TargetScheduler::registerTarget(const TargetId& targetId, const TargetPolicy& policy) {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    TargetPolicy p = policy;
    p.targetId = targetId;
    policies_[targetId] = p;
    lastDOMProcess_[targetId] = std::chrono::steady_clock::now();
    lastVisualProcess_[targetId] = std::chrono::steady_clock::now();

    LOG_INFO("TargetScheduler: registered target {} with priority {} mode {}",
             targetId.value, static_cast<int>(p.priority), static_cast<int>(p.captureMode));
}

void TargetScheduler::unregisterTarget(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    policies_.erase(targetId);
    lastDOMProcess_.erase(targetId);
    lastVisualProcess_.erase(targetId);

    LOG_INFO("TargetScheduler: unregistered target {}", targetId.value);
}

void TargetScheduler::updatePolicy(const TargetId& targetId, const TargetPolicy& policy) {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    if (policies_.contains(targetId)) {
        TargetPolicy p = policy;
        p.targetId = targetId;
        policies_[targetId] = p;
        LOG_DEBUG("TargetScheduler: updated policy for {}", targetId.value);
    }
}

TargetPolicy TargetScheduler::getPolicy(const TargetId& targetId) const {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    if (policies_.contains(targetId)) {
        return policies_.at(targetId);
    }
    return TargetPolicy{targetId};
}

void TargetScheduler::setTargetPriority(const TargetId& targetId, TargetPriority priority) {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    if (policies_.contains(targetId)) {
        policies_[targetId].priority = priority;
        LOG_DEBUG("TargetScheduler: set priority {} for {}", static_cast<int>(priority), targetId.value);
    }
}

void TargetScheduler::setCaptureMode(const TargetId& targetId, CaptureMode mode) {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    if (policies_.contains(targetId)) {
        policies_[targetId].captureMode = mode;
        LOG_DEBUG("TargetScheduler: set mode {} for {}", static_cast<int>(mode), targetId.value);
    }
}

void TargetScheduler::setFPS(const TargetId& targetId, double domFPS, double visualFPS) {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    if (policies_.contains(targetId)) {
        policies_[targetId].domFPS = std::max(0.1, std::min(domFPS, 30.0));
        policies_[targetId].visualFPS = std::max(0.1, std::min(visualFPS, 30.0));
        LOG_DEBUG("TargetScheduler: set FPS DOM={} visual={} for {}", domFPS, visualFPS, targetId.value);
    }
}

void TargetScheduler::pauseTarget(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    if (policies_.contains(targetId)) {
        policies_[targetId].paused = true;
        LOG_INFO("TargetScheduler: paused target {}", targetId.value);
    }
}

void TargetScheduler::resumeTarget(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    if (policies_.contains(targetId)) {
        policies_[targetId].paused = false;
        lastDOMProcess_[targetId] = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        lastVisualProcess_[targetId] = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        LOG_INFO("TargetScheduler: resumed target {}", targetId.value);
    }
}

void TargetScheduler::pauseAll() {
    std::lock_guard<std::mutex> lock(policiesMutex_);

    for (auto& [id, policy] : policies_) {
        policy.paused = true;
    }
    LOG_INFO("TargetScheduler: paused all {} targets", policies_.size());
}

void TargetScheduler::resumeAll() {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(policiesMutex_);

    for (auto& [id, policy] : policies_) {
        policy.paused = false;
        lastDOMProcess_[id] = now - std::chrono::seconds(10);
        lastVisualProcess_[id] = now - std::chrono::seconds(10);
    }
    LOG_INFO("TargetScheduler: resumed all {} targets", policies_.size());
}

void TargetScheduler::setDOMCallback(std::function<void(const TargetId&, const std::string&)> callback) {
    domCallback_ = std::move(callback);
}

void TargetScheduler::setVisualCallback(std::function<void(const TargetId&, const CapturedFrame&)> callback) {
    visualCallback_ = std::move(callback);
}

void TargetScheduler::setOCRCallback(std::function<void(const TargetId&, const OCRResult&)> callback) {
    ocrCallback_ = std::move(callback);
}

void TargetScheduler::setEmbeddingCallback(std::function<void(const TargetId&, const EmbeddingResult&)> callback) {
    embeddingCallback_ = std::move(callback);
}

void TargetScheduler::setErrorCallback(TargetCallback callback) {
    errorCallback_ = std::move(callback);
}

size_t TargetScheduler::activeTargets() const {
    std::lock_guard<std::mutex> lock(policiesMutex_);
    size_t active = 0;
    for (const auto& [id, policy] : policies_) {
        if (!policy.paused) active++;
    }
    return active;
}

size_t TargetScheduler::queueSize() const {
    return domQueue_.size() + visualQueue_.size() + ocrQueue_.size() + embeddingQueue_.size();
}

TargetScheduler::SchedulerStats TargetScheduler::getStats() const {
    SchedulerStats stats;
    stats.activeTargets = activeTargets();
    
    std::lock_guard<std::mutex> lock(policiesMutex_);
    stats.pausedTargets = policies_.size() - stats.activeTargets;
    
    stats.captureQueueSize = domQueue_.size() + visualQueue_.size();
    stats.ocrQueueSize = ocrQueue_.size();
    stats.embeddingQueueSize = embeddingQueue_.size();
    stats.totalDOMEvents = totalDOMEvents_.load();
    stats.totalVisualFrames = totalVisualFrames_.load();
    stats.totalOCREvents = totalOCREvents_.load();
    stats.totalEmbeddings = totalEmbeddings_.load();
    stats.cpuUsage = 0.0;
    
    return stats;
}

void TargetScheduler::resetStats() {
    totalDOMEvents_ = 0;
    totalVisualFrames_ = 0;
    totalOCREvents_ = 0;
    totalEmbeddings_ = 0;
}

void TargetScheduler::scheduleLoop() {
    while (running_) {
        std::vector<TargetId> toProcess;

        {
            std::lock_guard<std::mutex> lock(policiesMutex_);
            auto now = std::chrono::steady_clock::now();

            for (const auto& [id, policy] : policies_) {
                if (policy.paused) continue;
                if (policy.priority == TargetPriority::Inactive) continue;

                auto domIt = lastDOMProcess_.find(id);
                auto visIt = lastVisualProcess_.find(id);

                std::chrono::milliseconds domInterval(static_cast<int>(1000.0 / policy.domFPS));
                std::chrono::milliseconds visInterval(static_cast<int>(1000.0 / policy.visualFPS));

                bool domDue = (domIt == lastDOMProcess_.end()) ||
                    (now - domIt->second) >= domInterval;

                bool visDue = (visIt == lastVisualProcess_.end()) ||
                    (now - visIt->second) >= visInterval;

                if ((policy.captureMode == CaptureMode::DOM_Only || policy.captureMode == CaptureMode::DOM_and_Visual) && domDue) {
                    toProcess.push_back(id);
                    lastDOMProcess_[id] = now;
                }

                if ((policy.captureMode == CaptureMode::Visual_Only || policy.captureMode == CaptureMode::DOM_and_Visual) && visDue) {
                    toProcess.push_back(id);
                    lastVisualProcess_[id] = now;
                }
            }
        }

        if (config_.adaptiveScheduling && !toProcess.empty()) {
            adjustWorkerLoad();
        }

        for (const auto& targetId : toProcess) {
            if (!running_) break;
            capturePool_->enqueue([this, targetId]() {
                processTarget(targetId);
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void TargetScheduler::processTarget(const TargetId& targetId) {
    TargetPolicy policy;
    {
        std::lock_guard<std::mutex> lock(policiesMutex_);
        if (policies_.contains(targetId)) {
            policy = policies_[targetId];
        } else {
            return;
        }
    }

    if (policy.paused) return;

    try {
        switch (policy.captureMode) {
            case CaptureMode::DOM_Only:
                processDOM(targetId);
                break;
            case CaptureMode::Visual_Only:
                processVisual(targetId);
                break;
            case CaptureMode::DOM_and_Visual:
                processDOM(targetId);
                processVisual(targetId);
                break;
            case CaptureMode::None:
                break;
        }
    } catch (const std::exception& e) {
        handleTargetError(targetId, e.what());
    }
}

void TargetScheduler::processDOM(const TargetId& targetId) {
    if (!domCallback_) return;

    std::string text = "[DOM extraction placeholder - integrate with DOMExtractor]";

    totalDOMEvents_++;

    if (!domQueue_.push({targetId, text})) {
        LOG_WARN("TargetScheduler: DOM queue full, dropping for {}", targetId.value);
    }

    domCallback_(targetId, text);
}

void TargetScheduler::processVisual(const TargetId& targetId) {
    if (!visualCallback_) return;

    CapturedFrame frame;
    frame.targetId = targetId;
    frame.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    totalVisualFrames_++;

    if (!visualQueue_.push({targetId, frame})) {
        LOG_WARN("TargetScheduler: visual queue full, dropping for {}", targetId.value);
    }

    visualCallback_(targetId, frame);
}

void TargetScheduler::dispatchToOCR(const CapturedFrame& frame) {
    if (!ocrCallback_) return;

    ocrPool_->enqueue([this, frame]() {
        OCRResult result;
        result.targetId = frame.targetId;
        result.timestamp = frame.timestamp;
        result.text = "[OCR placeholder - integrate with OCRManager]";
        result.confidence = 0.0f;

        totalOCREvents_++;

        if (!ocrQueue_.push({frame.targetId, result})) {
            LOG_WARN("TargetScheduler: OCR queue full");
        }

        ocrCallback_(frame.targetId, result);
    });
}

void TargetScheduler::dispatchToEmbedding(const OCRResult& ocrResult) {
    if (!embeddingCallback_) return;

    embeddingPool_->enqueue([this, ocrResult]() {
        EmbeddingResult emb;
        emb.targetId = ocrResult.targetId;
        emb.timestamp = ocrResult.timestamp;
        emb.content = ocrResult.text;
        emb.source = ContentSource::OCR;
        emb.confidence = ocrResult.confidence;

        totalEmbeddings_++;
        embeddingCallback_(ocrResult.targetId, emb);
    });
}

void TargetScheduler::dispatchToEmbedding(const std::string& text, const Observation& obs) {
    if (!embeddingCallback_) return;

    embeddingPool_->enqueue([this, text, obs]() {
        EmbeddingResult emb;
        emb.targetId = obs.targetId;
        emb.timestamp = obs.timestamp;
        emb.content = text;
        emb.source = obs.source;
        emb.confidence = obs.confidence;
        emb.contentHash = obs.contentHash;

        totalEmbeddings_++;
        embeddingCallback_(obs.targetId, emb);
    });
}

void TargetScheduler::handleTargetError(const TargetId& targetId, const std::string& error) {
    LOG_ERROR("TargetScheduler: error processing {}: {}", targetId.value, error);

    if (errorCallback_) {
        errorCallback_(targetId, error);
    }
}

void TargetScheduler::adjustWorkerLoad() {
}

// ============================================================================
// TargetUI Implementation
// ============================================================================

TargetUI::TargetUI() = default;

TargetUI::~TargetUI() {
    shutdown();
}

void TargetUI::initialize() {
    LOG_DEBUG("TargetUI: initialized");
}

void TargetUI::shutdown() {
    targets_.clear();
    LOG_DEBUG("TargetUI: shutdown");
}

void TargetUI::addTarget(const TargetPolicy& policy) {
    TargetDisplay display;
    display.targetId = policy.targetId;
    display.state = TargetState::Discovered;
    display.priority = policy.priority;
    display.captureMode = policy.captureMode;
    display.ocrFPS = policy.visualFPS;
    display.embeddingCount = 0;
    display.cpuUsage = 0;
    display.gpuUsage = 0;

    targets_.push_back(display);
    LOG_INFO("TargetUI: added target {}", policy.targetId.value);
}

void TargetUI::removeTarget(const TargetId& targetId) {
    targets_.erase(
        std::remove_if(targets_.begin(), targets_.end(),
            [&targetId](const TargetDisplay& t) { return t.targetId == targetId; }),
        targets_.end()
    );
    LOG_INFO("TargetUI: removed target {}", targetId.value);
}

void TargetUI::updateTargetStatus(const TargetId& targetId, TargetState state) {
    for (auto& target : targets_) {
        if (target.targetId == targetId) {
            target.state = state;
            target.lastUpdate = std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
            break;
        }
    }
}

void TargetUI::updateTargetStats(const TargetId& targetId, const std::string& stat, double value) {
    for (auto& target : targets_) {
        if (target.targetId == targetId) {
            if (stat == "ocrFPS") target.ocrFPS = value;
            else if (stat == "embeddingCount") target.embeddingCount = static_cast<size_t>(value);
            else if (stat == "cpuUsage") target.cpuUsage = value;
            else if (stat == "gpuUsage") target.gpuUsage = value;
            break;
        }
    }
}

void TargetUI::updateTargetStats(const TargetId& targetId, const std::string& stat, const std::string& value) {
    for (auto& target : targets_) {
        if (target.targetId == targetId) {
            if (stat == "title") target.title = value;
            else if (stat == "url") target.url = PageUrl(value);
            else if (stat == "domStatus") target.domStatus = value;
            else if (stat == "visualStatus") target.visualStatus = value;
            else if (stat == "ocrStatus") target.ocrStatus = value;
            else if (stat == "lastUpdate") target.lastUpdate = value;
            break;
        }
    }
}

void TargetUI::setAddTargetCallback(std::function<void()> callback) {
    addTargetCallback_ = std::move(callback);
}

void TargetUI::setLockTargetCallback(std::function<void(const TargetId&)> callback) {
    lockTargetCallback_ = std::move(callback);
}

void TargetUI::setUnlockTargetCallback(std::function<void(const TargetId&)> callback) {
    unlockTargetCallback_ = std::move(callback);
}

void TargetUI::setPauseOCRCallback(std::function<void(const TargetId&)> callback) {
    pauseOCRCallback_ = std::move(callback);
}

void TargetUI::setResumeOCRCallback(std::function<void(const TargetId&)> callback) {
    resumeOCRCallback_ = std::move(callback);
}

void TargetUI::setRemoveTargetCallback(std::function<void(const TargetId&)> callback) {
    removeTargetCallback_ = std::move(callback);
}

std::vector<TargetUI::TargetDisplay> TargetUI::getAllTargets() const {
    return targets_;
}

TargetUI::TargetDisplay TargetUI::getTarget(const TargetId& targetId) const {
    for (const auto& target : targets_) {
        if (target.targetId == targetId) {
            return target;
        }
    }
    return TargetDisplay{};
}

void TargetUI::render() {
    std::cout << "\033[2J\033[H";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    Edge Intelligence - Target Monitor                         ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";

    for (const auto& target : targets_) {
        renderTarget(target);
    }

    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\nActions: [A]dd [L]ock [U]nlock [P]auseOCR [R]esumeOCR [X]Remove\n";
}

void TargetUI::renderTarget(const TargetDisplay& target) {
    std::string stateStr;
    switch (target.state) {
        case TargetState::Discovered: stateStr = "DISCOVERED"; break;
        case TargetState::Connecting: stateStr = "CONNECTING"; break;
        case TargetState::Connected: stateStr = "CONNECTED"; break;
        case TargetState::Monitoring: stateStr = "MONITORING"; break;
        case TargetState::Navigating: stateStr = "NAVIGATING"; break;
        case TargetState::Reconnecting: stateStr = "RECONNECTING"; break;
        case TargetState::Offline: stateStr = "OFFLINE"; break;
        case TargetState::Closed: stateStr = "CLOSED"; break;
        case TargetState::Error: stateStr = "ERROR"; break;
        default: stateStr = "UNKNOWN";
    }

    std::cout << "║ " << target.targetId.value << "\n";
    std::cout << "║   Title: " << (target.title.empty() ? "(none)" : target.title) << "\n";
    std::cout << "║   URL: " << (target.url.value.empty() ? "(none)" : target.url.value) << "\n";
    std::cout << "║   Status: " << stateStr;
    std::cout << " | DOM: " << (target.domStatus.empty() ? "-" : target.domStatus);
    std::cout << " | Visual: " << (target.visualStatus.empty() ? "-" : target.visualStatus);
    std::cout << " | OCR: " << (target.ocrStatus.empty() ? "-" : target.ocrStatus) << "\n";
    std::cout << "║   OCR FPS: " << target.ocrFPS << " | Embeddings: " << target.embeddingCount;
    std::cout << " | CPU: " << target.cpuUsage << "% | GPU: " << target.gpuUsage << "%\n";
    std::cout << "║   Priority: " << (target.priority == TargetPriority::High ? "HIGH" :
                               target.priority == TargetPriority::Normal ? "NORMAL" :
                               target.priority == TargetPriority::Low ? "LOW" : "INACTIVE") << "\n";
    std::cout << "║   Mode: " << (target.captureMode == CaptureMode::DOM_and_Visual ? "DOM+Visual" :
                               target.captureMode == CaptureMode::DOM_Only ? "DOM Only" :
                               target.captureMode == CaptureMode::Visual_Only ? "Visual Only" : "None") << "\n";
    std::cout << "╠──────────────────────────────────────────────────────────────────────────────╣\n";
}

} // namespace edgemon
