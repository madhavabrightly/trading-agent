#include "trading/strategy/Strategies.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace trading {

namespace {

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

constexpr size_t kMaxCloses = 200;

} // namespace

// ---------------------------------------------------------------------------
// TechnicalMomentumStrategy
// ---------------------------------------------------------------------------

void TechnicalMomentumStrategy::addClose(const std::string& symbol,
                                         double close) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s = series_[symbol];
    s.closes.push_back(close);
    if (s.closes.size() > kMaxCloses) s.closes.pop_front();

    // EMA-9 / EMA-21 over the available series.
    auto ema = [](const std::deque<double>& c, size_t period) {
        if (c.empty()) return 0.0;
        double alpha = 2.0 / (period + 1.0);
        double e = c.front();
        for (size_t i = 1; i < c.size(); ++i)
            e = alpha * c[i] + (1.0 - alpha) * e;
        return e;
    };
    s.emaFast = ema(s.closes, 9);
    s.emaSlow = ema(s.closes, 21);
    s.emaInit = s.closes.size() >= 21;
}

void TechnicalMomentumStrategy::on_market(const MarketTick& tick) {
    if (tick.last > 0.0) addClose(tick.symbol, tick.last);
}

std::optional<TechnicalMomentumStrategy::Score> TechnicalMomentumStrategy::score(
    const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = series_.find(symbol);
    if (it == series_.end() || !it->second.emaInit) return std::nullopt;
    Score s;
    s.emaFast = it->second.emaFast;
    s.emaSlow = it->second.emaSlow;
    s.momentum = it->second.emaSlow > 0.0
                     ? it->second.emaFast / it->second.emaSlow - 1.0
                     : 0.0;
    return s;
}

std::optional<TradingSignal> TechnicalMomentumStrategy::generate_signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Emit for the symbol with the strongest |momentum| above threshold.
    double best = 0.0;
    std::string bestSym;
    double bestEmaFast = 0.0, bestEmaSlow = 0.0;
    for (const auto& [sym, s] : series_) {
        if (!s.emaInit) continue;
        double m = s.emaSlow > 0.0 ? s.emaFast / s.emaSlow - 1.0 : 0.0;
        if (std::fabs(m) > std::fabs(best)) {
            best = m;
            bestSym = sym;
            bestEmaFast = s.emaFast;
            bestEmaSlow = s.emaSlow;
        }
    }
    if (bestSym.empty() || std::fabs(best) < 0.001) return std::nullopt;

    TradingSignal sig;
    sig.symbol = bestSym;
    sig.direction = best > 0.0 ? TradingSignal::Direction::LONG
                               : TradingSignal::Direction::SHORT;
    // Confidence scales with momentum strength, capped.
    sig.confidence = std::min(0.9, 0.3 + std::fabs(best) * 8.0);
    sig.entry = bestEmaFast;
    sig.strategy = "technical";
    sig.timestamp = nowNs();
    sig.expectedValue = best;
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "EMA9 %.4f vs EMA21 %.4f momentum %+.4f", bestEmaFast,
                  bestEmaSlow, best);
    sig.reason = buf;
    return sig;
}

void TechnicalMomentumStrategy::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    series_.clear();
}

// ---------------------------------------------------------------------------
// NewsOcrStrategy
// ---------------------------------------------------------------------------

void NewsOcrStrategy::on_ocr(const OcrEvent& event) {
    // Only consider content with a directional lean.
    if (event.type != OcrContentType::NEWS &&
        event.type != OcrContentType::ALERT &&
        event.type != OcrContentType::ECONOMIC)
        return;
    if (event.sentiment == 0.0 && !event.urgent) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Event e;
    e.symbol = event.symbols.empty() ? "" : event.symbols[0];
    e.score = event.sentiment;   // + positive, - negative
    e.confidence = event.confidence;
    e.ts = event.timestampNs;
    e.text = event.text;
    if (e.text.size() > 200) e.text.resize(200);
    recent_.push_back(std::move(e));
    if (recent_.size() > 32) recent_.pop_front();
}

void NewsOcrStrategy::on_news(const std::string& text, const std::string& asset,
                              double sentiment) {
    if (sentiment == 0.0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Event e;
    e.symbol = asset;
    e.score = sentiment;
    e.confidence = 0.8;
    e.ts = nowNs();
    e.text = text;
    if (e.text.size() > 200) e.text.resize(200);
    recent_.push_back(std::move(e));
    if (recent_.size() > 32) recent_.pop_front();
}

std::optional<TradingSignal> NewsOcrStrategy::generate_signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Look at the newest event within the last 10 minutes.
    uint64_t now = nowNs();
    const uint64_t kWindow = 600ULL * 1'000'000'000ULL;  // 10 min
    // Only one emission per 60s to avoid signal spam from the same event.
    if (now - lastEmitTs_ < 60ULL * 1'000'000'000ULL) return std::nullopt;

    for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
        if (now - it->ts > kWindow) break;
        if (std::fabs(it->score) < 0.25) continue;
        TradingSignal sig;
        sig.symbol = it->symbol;
        sig.direction = it->score > 0.0 ? TradingSignal::Direction::LONG
                                        : TradingSignal::Direction::SHORT;
        sig.confidence = std::min(0.85, it->confidence * 0.9 + 0.2);
        sig.strategy = "news_ocr";
        sig.timestamp = now;
        sig.expectedValue = it->score;
        sig.reason = "OCR/news: " + it->text;
        sig.urgent = std::fabs(it->score) > 0.6;
        lastEmitTs_ = now;
        return sig;
    }
    return std::nullopt;
}

void NewsOcrStrategy::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_.clear();
    lastEmitTs_ = 0;
}

} // namespace trading
