#pragma once

#include "common.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace edgemon {

class ChangeDetector {
public:
    ChangeDetector() = default;

    bool detect(const CapturedFrame& frame);
    
    void setSimilarityThreshold(float threshold) { threshold_ = threshold; }
    float getSimilarityThreshold() const { return threshold_; }
    
    void reset(const std::string& targetId);
    void resetAll();

    struct DetectionResult {
        bool hasChanged;
        float similarity;
        std::string previousHash;
        std::string currentHash;
    };

    DetectionResult getLastResult(const std::string& targetId) const;

private:
    std::string computeFrameHash(const CapturedFrame& frame);
    float computeSimilarity(const CapturedFrame& frame1, const CapturedFrame& frame2);

    float threshold_ = 0.95f;
    
    struct FrameState {
        std::string hash;
        CapturedFrame lastFrame;
        DetectionResult lastResult;
    };
    
    std::unordered_map<std::string, FrameState> targetStates_;
    mutable std::mutex mutex_;
};

}
