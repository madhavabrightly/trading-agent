#pragma once

// trading/signal/SignalFusion.hpp — combines signals from independent sources
// into one weighted decision (spec §18). No single source is trusted blindly;
// weights are configurable.

#include "trading/signal/TradingSignal.hpp"
#include <optional>
#include <string>
#include <vector>

namespace trading {

// One scored directional vote from a source. A source may vote LONG (+),
// SHORT (-) or abstain (0).
struct SignalVote {
    std::string source;      // "technical", "orderbook", "news", "ai", ...
    double score = 0.0;      // -1..1 (sign = direction, magnitude = strength)
    double confidence = 0.0; // 0..1
    std::string reason;
};

class SignalFusion {
public:
    SignalFusion() = default;
    explicit SignalFusion(FusionWeights weights);

    // Combines votes into a fused directional signal for a symbol.
    // Returns nullopt when the fused strength is below minStrength or votes
    // cancel out (|score| < threshold).
    std::optional<FusedSignal> fuse(
        const std::string& symbol, const std::vector<SignalVote>& votes,
        double minStrength = 0.15) const;

    void setWeights(const FusionWeights& w) { weights_ = w; }
    FusionWeights weights() const { return weights_; }

    // Maps a directional score to the strategy direction enum.
    static TradingSignal::Direction toDirection(double score);

private:
    FusionWeights weights_;
};

} // namespace trading
