#include "capture/ChangeDetector.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace edgemon {

FrameChangeDetector::FrameChangeDetector() = default;

FrameChangeDetector::FrameChangeDetector(const Config& config) : config_(config) {}

FrameChangeDetector::~FrameChangeDetector() = default;

void FrameChangeDetector::setConfig(const Config& config) {
    config_ = config;
}

FrameChangeDetector::Config FrameChangeDetector::getConfig() const {
    return config_;
}

FrameChangeDetector::Stats FrameChangeDetector::getStats() const {
    return {
        framesProcessed_.load(),
        framesChanged_.load(),
        framesSkipped_.load(),
        regionChanges_.load()
    };
}

void FrameChangeDetector::resetStats() {
    framesProcessed_ = 0;
    framesChanged_ = 0;
    framesSkipped_ = 0;
    regionChanges_ = 0;
}

void FrameChangeDetector::reset() {
    previousPixels_.clear();
    previousWidth_ = 0;
    previousHeight_ = 0;
    previousHash_ = 0;
    resetStats();
}

void FrameChangeDetector::setPreviousFrame(const CapturedFrame& frame) {
    if (frame.pixels.empty()) return;
    
    previousPixels_ = frame.pixels;
    previousWidth_ = frame.width;
    previousHeight_ = frame.height;
    previousHash_ = computePerceptualHash(frame.pixels, frame.width, frame.height);
}

uint64_t FrameChangeDetector::computePerceptualHash(const std::vector<uint8_t>& pixels,
                                                   uint32_t width, uint32_t height) {
    if (pixels.empty() || width == 0 || height == 0) {
        return 0;
    }

    constexpr uint32_t HASH_SIZE = 8;
    constexpr uint32_t DOWNSAMPLE_W = HASH_SIZE * 8;
    constexpr uint32_t DOWNSAMPLE_H = HASH_SIZE * 8;
    
    std::vector<uint32_t> blocks(HASH_SIZE * HASH_SIZE, 0);
    size_t blockPixels = 0;
    
    uint32_t srcW = std::min(width, DOWNSAMPLE_W);
    uint32_t srcH = std::min(height, DOWNSAMPLE_H);
    
    uint32_t blockW = (srcW + HASH_SIZE - 1) / HASH_SIZE;
    uint32_t blockH = (srcH + HASH_SIZE - 1) / HASH_SIZE;
    
    for (uint32_t y = 0; y < srcH; ++y) {
        for (uint32_t x = 0; x < srcW; ++x) {
            size_t idx = (y * width + x) * 4;
            if (idx + 2 >= pixels.size()) continue;
            
            uint8_t r = pixels[idx];
            uint8_t g = pixels[idx + 1];
            uint8_t b = pixels[idx + 2];
            
            uint32_t gray = (static_cast<uint32_t>(r) * 299 + 
                           static_cast<uint32_t>(g) * 587 + 
                           static_cast<uint32_t>(b) * 114) / 1000;
            
            uint32_t bx = (x * HASH_SIZE) / srcW;
            uint32_t by = (y * HASH_SIZE) / srcH;
            
            blocks[by * HASH_SIZE + bx] += gray;
            blockPixels++;
        }
    }
    
    if (blockPixels > 0) {
        for (auto& block : blocks) {
            block /= blockPixels;
        }
    }
    
    uint64_t hash = 0x1234567890ABCDEFULL;
    
    for (uint32_t by = 0; by < HASH_SIZE; ++by) {
        for (uint32_t bx = 0; bx < HASH_SIZE; ++bx) {
            uint32_t idx = by * HASH_SIZE + bx;
            uint32_t blockVal = blocks[idx];
            
            uint32_t avgVal = 0;
            uint32_t count = 0;
            for (uint32_t dy = 0; dy < HASH_SIZE; ++dy) {
                for (uint32_t dx = 0; dx < HASH_SIZE; ++dx) {
                    uint32_t neighborIdx = dy * HASH_SIZE + dx;
                    if (neighborIdx < blocks.size()) {
                        avgVal += blocks[neighborIdx];
                        count++;
                    }
                }
            }
            if (count > 0) avgVal /= count;
            
            if (blockVal > avgVal) {
                uint32_t bitPos = (by * HASH_SIZE + bx);
                hash |= (1ULL << bitPos);
            }
        }
    }
    
    return hash;
}

RegionDiff FrameChangeDetector::computeRegionDiff(const CapturedFrame& current,
                                                 const CapturedFrame& previous,
                                                 uint32_t regionX, uint32_t regionY,
                                                 uint32_t regionW, uint32_t regionH) {
    RegionDiff diff;
    diff.x = regionX;
    diff.y = regionY;
    diff.width = regionW;
    diff.height = regionH;
    
    if (previousPixels_.empty() || current.pixels.empty()) {
        diff.hasChanged = true;
        return diff;
    }
    
    size_t changedPixels = 0;
    size_t totalCompared = 0;
    size_t totalDiff = 0;
    
    uint32_t maxX = std::min(regionX + regionW, std::min(current.width, previousWidth_));
    uint32_t maxY = std::min(regionY + regionH, std::min(current.height, previousHeight_));
    
    for (uint32_t y = regionY; y < maxY; ++y) {
        for (uint32_t x = regionX; x < maxX; ++x) {
            size_t currentIdx = (y * current.width + x) * 4;
            size_t previousIdx = (y * previousWidth_ + x) * 4;
            
            if (currentIdx + 2 >= current.pixels.size() ||
                previousIdx + 2 >= previousPixels_.size()) {
                continue;
            }
            
            int dr = static_cast<int>(current.pixels[currentIdx]) - 
                     static_cast<int>(previousPixels_[previousIdx]);
            int dg = static_cast<int>(current.pixels[currentIdx + 1]) - 
                     static_cast<int>(previousPixels_[previousIdx + 1]);
            int db = static_cast<int>(current.pixels[currentIdx + 2]) - 
                     static_cast<int>(previousPixels_[previousIdx + 2]);
            
            int diff = (dr * dr + dg * dg + db * db);
            
            if (diff > 100) {
                changedPixels++;
            }
            
            totalDiff += diff;
            totalCompared++;
        }
    }
    
    diff.changedPixels = changedPixels;
    
    if (totalCompared > 0) {
        diff.similarity = 1.0f - (static_cast<float>(totalDiff) / 
                                   (totalCompared * 255 * 255 * 3));
    }
    
    uint32_t regionPixels = regionW * regionH;
    float changeRatio = static_cast<float>(changedPixels) / 
                       (regionPixels > 0 ? regionPixels : 1);
    
    diff.hasChanged = changedPixels >= config_.minChangedPixels && 
                     changeRatio > (1.0f - config_.similarityThreshold);
    
    if (diff.hasChanged) {
        regionChanges_++;
    }
    
    return diff;
}

FrameChangeResult FrameChangeDetector::detect(const CapturedFrame& frame) {
    FrameChangeResult result;
    result.timestamp = frame.timestamp;
    
    framesProcessed_++;
    
    if (frame.pixels.empty() || frame.status != CaptureStatus::Success) {
        result.hasChanged = false;
        framesSkipped_++;
        return result;
    }
    
    if (previousPixels_.empty()) {
        previousPixels_ = frame.pixels;
        previousWidth_ = frame.width;
        previousHeight_ = frame.height;
        previousHash_ = computePerceptualHash(frame.pixels, frame.width, frame.height);
        
        result.hasChanged = true;
        result.frameHash = previousHash_;
        framesChanged_++;
        
        RegionDiff fullRegion;
        fullRegion.x = 0;
        fullRegion.y = 0;
        fullRegion.width = frame.width;
        fullRegion.height = frame.height;
        fullRegion.hasChanged = true;
        result.regions.push_back(fullRegion);
        
        return result;
    }
    
    uint64_t currentHash = computePerceptualHash(frame.pixels, frame.width, frame.height);
    result.frameHash = currentHash;
    
    // Perceptual hash collisions are possible (e.g. uniform fills). The pixel
    // difference is authoritative; the hash is only a fast-path for identical
    // content, so we always fall through to pixel comparison below.
    if (currentHash == previousHash_ && frame.pixels == previousPixels_) {
        result.hasChanged = false;
        result.overallSimilarity = 1.0f;
        framesSkipped_++;
        return result;
    }
    
    float pixelDiff = 0.0f;
    size_t comparedPixels = 0;
    
    uint32_t maxW = std::min(frame.width, previousWidth_);
    uint32_t maxH = std::min(frame.height, previousHeight_);
    
    for (uint32_t y = 0; y < maxH; ++y) {
        for (uint32_t x = 0; x < maxW; ++x) {
            size_t currentIdx = (y * frame.width + x) * 4;
            size_t previousIdx = (y * previousWidth_ + x) * 4;
            
            if (currentIdx + 2 >= frame.pixels.size() ||
                previousIdx + 2 >= previousPixels_.size()) {
                continue;
            }
            
            int dr = static_cast<int>(frame.pixels[currentIdx]) - 
                     static_cast<int>(previousPixels_[previousIdx]);
            int dg = static_cast<int>(frame.pixels[currentIdx + 1]) - 
                     static_cast<int>(previousPixels_[previousIdx + 1]);
            int db = static_cast<int>(frame.pixels[currentIdx + 2]) - 
                     static_cast<int>(previousPixels_[previousIdx + 2]);
            
            pixelDiff += std::sqrt(static_cast<float>(dr * dr + dg * dg + db * db));
            comparedPixels++;
        }
    }
    
    if (comparedPixels > 0) {
        result.overallSimilarity = 1.0f - (pixelDiff / (comparedPixels * 441.67f));
    }
    
    if (result.overallSimilarity >= config_.similarityThreshold) {
        result.hasChanged = false;
        framesSkipped_++;
        return result;
    }
    
    result.hasChanged = true;
    framesChanged_++;
    
    if (config_.enableRegionDetection) {
        uint32_t regionW = std::max(config_.regionSize, frame.width / 8);
        uint32_t regionH = std::max(config_.regionSize, frame.height / 8);
        
        CapturedFrame previousFrame;
        previousFrame.pixels = previousPixels_;
        previousFrame.width = previousWidth_;
        previousFrame.height = previousHeight_;
        
        for (uint32_t y = 0; y < frame.height; y += regionH) {
            for (uint32_t x = 0; x < frame.width; x += regionW) {
                uint32_t rw = std::min(regionW, frame.width - x);
                uint32_t rh = std::min(regionH, frame.height - y);
                
                auto regionDiff = computeRegionDiff(frame, previousFrame, x, y, rw, rh);
                
                if (regionDiff.hasChanged) {
                    result.regions.push_back(regionDiff);
                }
            }
        }
    }
    
    previousPixels_ = frame.pixels;
    previousWidth_ = frame.width;
    previousHeight_ = frame.height;
    previousHash_ = currentHash;
    
    return result;
}

bool FrameChangeDetector::hasFrameChanged(const CapturedFrame& frame) {
    auto result = detect(frame);
    return result.hasChanged;
}

std::vector<RegionDiff> FrameChangeDetector::getChangedRegions(const CapturedFrame& frame) {
    auto result = detect(frame);
    return result.regions;
}

} // namespace edgemon
