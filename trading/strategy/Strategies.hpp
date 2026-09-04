#pragma once

// trading/strategy/Strategies.hpp — starter strategy plugins (spec §45):
//   * TechnicalMomentumStrategy — EMA crossover + RSI over OHLCV bars
//   * NewsOcrStrategy          — validated OCR/news events -> directional vote
// Both are pure signal generators; they never touch connectors.

#include "trading/signal/TradingSignal.hpp"
#include "trading/ocr/OcrTypes.hpp"

#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trading {

// Technical strategy: tracks per-symbol close prices (fed by market ticks /
// bars), computes EMA(9) vs EMA(21) and a momentum score. Emits a signal when
// the crossover/momentum crosses a threshold.
class TechnicalMomentumStrategy : public IStrategy {
public:
    TechnicalMomentumStrategy() = default;

    std::string name() const override { return "technical"; }

    void on_market(const MarketTick& tick) override;
    void on_ocr(const OcrEvent&) override {}
    void on_news(const std::string&, const std::string&, double) override {}
    void on_position(const Position&) override {}

    std::optional<TradingSignal> generate_signal() override;
    void reset() override;

    // Unit-test hook: inject close prices directly.
    void addClose(const std::string& symbol, double close);

    struct Score {
        double emaFast = 0.0;
        double emaSlow = 0.0;
        double momentum = 0.0;   // normalized (last/emaSlow - 1)
    };
    std::optional<Score> score(const std::string& symbol) const;

private:
    struct Series {
        std::deque<double> closes;      // ring buffer, newest last
        double emaFast = 0.0;
        double emaSlow = 0.0;
        bool emaInit = false;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Series> series_;
};

// News/OCR strategy: converts validated classified OCR events into votes.
// Pure function of recent events — dedup/staleness handled upstream.
class NewsOcrStrategy : public IStrategy {
public:
    NewsOcrStrategy() = default;

    std::string name() const override { return "news_ocr"; }

    void on_market(const MarketTick&) override {}
    void on_ocr(const OcrEvent& event) override;
    void on_news(const std::string& text, const std::string& asset,
                 double sentiment) override;
    void on_position(const Position&) override {}

    std::optional<TradingSignal> generate_signal() override;
    void reset() override;

private:
    struct Event {
        std::string symbol;
        double score = 0.0;      // signed directional lean
        double confidence = 0.0;
        uint64_t ts = 0;
        std::string text;
    };
    mutable std::mutex mutex_;
    std::deque<Event> recent_;   // newest last
    uint64_t lastEmitTs_ = 0;
};

} // namespace trading
