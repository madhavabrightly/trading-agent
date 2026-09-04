#pragma once

// trading/signal/TradingSignal.hpp — unified signal format (spec §17) and the
// strategy plugin interface (spec §45). Strategies receive events and emit
// signals; they never call broker APIs directly.

#include "trading/core/Types.hpp"
#include "trading/market/MarketTypes.hpp"
#include "trading/ocr/OcrTypes.hpp"
#include "trading/connectors/TradingTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace trading {

// One directional recommendation (spec §17). entry/sl/tp may be 0 for
// market-order signals where the router derives them.
struct TradingSignal {
    std::string symbol;

    enum class Direction { LONG, SHORT, FLAT };
    Direction direction = Direction::FLAT;

    double confidence = 0.0;      // 0..1 fused confidence
    double entry = 0.0;
    double stopLoss = 0.0;
    double takeProfit = 0.0;

    double expectedValue = 0.0;   // per-unit expected value in quote currency
    double quantity = 0.0;        // risk-approved later; 0 = size by risk

    std::string strategy;         // "technical", "news", "ocr", "fusion", ...
    std::string reason;           // human readable
    std::string source;           // component that created it

    uint64_t timestamp = 0;       // ns epoch
    bool urgent = false;
};

constexpr const char* toString(TradingSignal::Direction d) {
    switch (d) {
        case TradingSignal::Direction::LONG:  return "LONG";
        case TradingSignal::Direction::SHORT: return "SHORT";
        case TradingSignal::Direction::FLAT:  return "FLAT";
    }
    return "FLAT";
}

// Strategy plugin interface (spec §45). A strategy is pure signal generation —
// it observes events, maintains its own state, and produces optional signals.
class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual std::string name() const = 0;

    virtual void on_market(const MarketTick& tick) = 0;
    virtual void on_ocr(const OcrEvent& event) = 0;
    virtual void on_news(const std::string& text, const std::string& asset,
                         double sentiment) = 0;
    virtual void on_position(const Position& position) = 0;

    // Called on a timer / after each event batch. Returns a signal when the
    // strategy has a decision, otherwise nullopt.
    virtual std::optional<TradingSignal> generate_signal() = 0;

    virtual void reset() = 0;
};

// Signal source weights for fusion (spec §18). Configurable.
struct FusionWeights {
    double technical = 0.30;
    double orderbook = 0.15;
    double news = 0.20;
    double ai = 0.20;
    double sentiment = 0.15;
};

struct FusedSignal {
    TradingSignal signal;
    // component scores for observability
    double technicalScore = 0.0;
    double orderbookScore = 0.0;
    double newsScore = 0.0;
    double aiScore = 0.0;
    double sentimentScore = 0.0;
};

} // namespace trading
