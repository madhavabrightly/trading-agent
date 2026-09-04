// Unit tests for signal fusion + starter strategies (Phase 10).
#include "trading/signal/SignalFusion.hpp"
#include "trading/strategy/Strategies.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace trading;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

int main() {
    SignalFusion fusion;

    // All sources agree LONG.
    std::vector<SignalVote> bullish = {
        {"technical", 0.70, 0.8, "ema cross"},
        {"orderbook", 0.55, 0.7, ""},
        {"news", 0.80, 0.9, "etf inflows"},
        {"ai", 0.65, 0.7, ""},
        {"sentiment", 0.60, 0.6, ""},
    };
    auto f = fusion.fuse("BTC/USD", bullish, 0.15);
    CHECK(f.has_value(), "bullish votes produce a fused signal");
    if (f) {
        CHECK(f->signal.direction == TradingSignal::Direction::LONG,
              "fused direction LONG");
        CHECK(f->signal.confidence > 0.6, "fused confidence high");
    }

    // Conflicting votes cancel out.
    std::vector<SignalVote> mixed = {
        {"technical", 0.90, 0.9, ""},
        {"news", -0.90, 0.9, ""},
        {"ai", 0.0, 0.0, ""},
    };
    auto g = fusion.fuse("ETH/USD", mixed, 0.3);
    CHECK(!g.has_value(), "cancelling votes produce no signal above threshold");

    // One weak source alone stays below minStrength.
    std::vector<SignalVote> weak = {{"sentiment", 0.1, 0.5, ""}};
    auto h = fusion.fuse("SOL/USD", weak, 0.15);
    CHECK(!h.has_value(), "weak single vote below threshold");

    // Direction mapping.
    CHECK(SignalFusion::toDirection(0.5) == TradingSignal::Direction::LONG,
          "positive -> LONG");
    CHECK(SignalFusion::toDirection(-0.5) == TradingSignal::Direction::SHORT,
          "negative -> SHORT");
    CHECK(SignalFusion::toDirection(0.0) == TradingSignal::Direction::FLAT,
          "zero -> FLAT");

    // Technical strategy: rising closes => LONG signal.
    TechnicalMomentumStrategy tech;
    double p = 100.0;
    for (int i = 0; i < 60; ++i) {
        p += (i % 3 == 0) ? 0.5 : -0.1;  // net rising
        tech.addClose("BTC/USD", p);
    }
    auto tsig = tech.generate_signal();
    CHECK(tsig.has_value(), "technical strategy emits on rising series");
    if (tsig) {
        CHECK(tsig->symbol == "BTC/USD", "technical signal symbol");
        CHECK(tsig->direction == TradingSignal::Direction::LONG,
              "rising momentum => LONG");
    }

    // Technical strategy: falling closes => SHORT.
    TechnicalMomentumStrategy tech2;
    p = 200.0;
    for (int i = 0; i < 60; ++i) {
        p -= 0.6;
        tech2.addClose("ETH/USD", p);
    }
    auto tsig2 = tech2.generate_signal();
    CHECK(tsig2.has_value(), "technical strategy emits on falling series");
    if (tsig2) {
        CHECK(tsig2->direction == TradingSignal::Direction::SHORT,
              "falling momentum => SHORT");
    }

    // Flat/no data => no signal.
    TechnicalMomentumStrategy tech3;
    CHECK(!tech3.generate_signal().has_value(), "no data => no signal");

    // News/OCR strategy.
    NewsOcrStrategy news;
    OcrEvent ev;
    ev.type = OcrContentType::NEWS;
    ev.sentiment = 0.8;
    ev.confidence = 0.9;
    ev.timestampNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    ev.text = "BTC surges on ETF inflows";
    ev.symbols = {"BTC"};
    news.on_ocr(ev);
    auto nsig = news.generate_signal();
    CHECK(nsig.has_value(), "news strategy emits on positive OCR event");
    if (nsig) {
        CHECK(nsig->direction == TradingSignal::Direction::LONG,
              "positive sentiment => LONG");
        CHECK(nsig->strategy == "news_ocr", "news strategy name");
    }
    // Immediate re-emit suppressed by 60s cooldown.
    CHECK(!news.generate_signal().has_value(),
          "cooldown suppresses signal spam");

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
