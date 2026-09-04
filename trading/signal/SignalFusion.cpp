#include "trading/signal/SignalFusion.hpp"

#include <cmath>
#include <numeric>

namespace trading {

namespace {

double weightFor(const FusionWeights& w, const std::string& source) {
    if (source == "technical") return w.technical;
    if (source == "orderbook") return w.orderbook;
    if (source == "news") return w.news;
    if (source == "ai") return w.ai;
    if (source == "sentiment") return w.sentiment;
    return 0.05;  // unknown sources carry minimal weight
}

} // namespace

SignalFusion::SignalFusion(FusionWeights weights) : weights_(weights) {}

TradingSignal::Direction SignalFusion::toDirection(double score) {
    if (score > 0.0) return TradingSignal::Direction::LONG;
    if (score < 0.0) return TradingSignal::Direction::SHORT;
    return TradingSignal::Direction::FLAT;
}

std::optional<FusedSignal> SignalFusion::fuse(
    const std::string& symbol, const std::vector<SignalVote>& votes,
    double minStrength) const {
    if (votes.empty()) return std::nullopt;

    FusedSignal out;
    out.signal.symbol = symbol;
    out.signal.strategy = "fusion";
    out.signal.timestamp = 0;  // set by caller

    // Weighted sum of signed scores, each scaled by its own confidence.
    double totalWeight = 0.0;
    double weightedScore = 0.0;
    double totalConfidence = 0.0;
    std::string reasons;

    auto addScore = [&](const std::string& key, double score, double& field) {
        double w = weightFor(weights_, key);
        if (w <= 0.0) return;
        field = score;
        weightedScore += w * score;
        totalWeight += w;
    };

    for (const auto& v : votes) {
        double w = weightFor(weights_, v.source);
        if (w <= 0.0) continue;
        double s = v.score * v.confidence;
        weightedScore += w * s;
        totalWeight += w;
        totalConfidence += v.confidence * w;
        if (v.source == "technical") out.technicalScore = v.score;
        else if (v.source == "orderbook") out.orderbookScore = v.score;
        else if (v.source == "news") out.newsScore = v.score;
        else if (v.source == "ai") out.aiScore = v.score;
        else if (v.source == "sentiment") out.sentimentScore = v.score;

        if (!reasons.empty()) reasons += "; ";
        reasons += v.source + "=" + std::to_string(v.score);
        if (!v.reason.empty()) reasons += "(" + v.reason + ")";
    }

    if (totalWeight <= 0.0) return std::nullopt;
    double fused = weightedScore / totalWeight;
    double confidence = totalWeight > 0.0 ? totalConfidence / totalWeight : 0.0;

    if (std::fabs(fused) < minStrength) return std::nullopt;

    out.signal.direction = toDirection(fused);
    out.signal.confidence = std::min(1.0, std::max(0.0, confidence));
    out.signal.expectedValue = fused;  // proxy: router/risk translate to units
    out.signal.reason = reasons;

    // Urgency when the vote is strong and confident.
    out.signal.urgent = std::fabs(fused) > 0.6 && confidence > 0.7;
    return out;
}

} // namespace trading
