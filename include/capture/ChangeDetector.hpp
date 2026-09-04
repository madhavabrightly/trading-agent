#pragma once

#include "core/Types.hpp"
#include "capture/BrowserCapture.hpp"
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

namespace edgemon {

struct RegionDiff {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float similarity = 0.0f;
    size_t changedPixels = 0;
    bool hasChanged = false;
};

struct FrameChangeResult {
    bool hasChanged = false;
    float overallSimilarity = 0.0f;
    std::vector<RegionDiff> regions;
    uint64_t frameHash = 0;
    uint64_t timestamp = 0;
};

class FrameChangeDetector {
public:
    struct Config {
        float similarityThreshold = 0.95f;
        uint32_t regionSize = 64;
        uint32_t minChangedPixels = 100;
        bool enableRegionDetection = true;
    };

    explicit FrameChangeDetector();
    explicit FrameChangeDetector(const Config& config);
    ~FrameChangeDetector();

    void setConfig(const Config& config);
    Config getConfig() const;

    FrameChangeResult detect(const CapturedFrame& frame);
    
    bool hasFrameChanged(const CapturedFrame& frame);
    std::vector<RegionDiff> getChangedRegions(const CapturedFrame& frame);
    
    uint64_t computePerceptualHash(const std::vector<uint8_t>& pixels, 
                                   uint32_t width, uint32_t height);
    RegionDiff computeRegionDiff(const CapturedFrame& current, 
                                 const CapturedFrame& previous,
                                 uint32_t regionX, uint32_t regionY,
                                 uint32_t regionW, uint32_t regionH);
    
    void reset();
    void setPreviousFrame(const CapturedFrame& frame);

    struct Stats {
        uint64_t framesProcessed = 0;
        uint64_t framesChanged = 0;
        uint64_t framesSkipped = 0;
        uint64_t regionChanges = 0;
    };
    Stats getStats() const;
    void resetStats();

private:
    Config config_;
    std::vector<uint8_t> previousPixels_;
    uint32_t previousWidth_ = 0;
    uint32_t previousHeight_ = 0;
    uint64_t previousHash_ = 0;
    
    std::atomic<uint64_t> framesProcessed_{0};
    std::atomic<uint64_t> framesChanged_{0};
    std::atomic<uint64_t> framesSkipped_{0};
    std::atomic<uint64_t> regionChanges_{0};
};

} // namespace edgemon
