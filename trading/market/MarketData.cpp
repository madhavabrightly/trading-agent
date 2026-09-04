#include "trading/market/MarketData.hpp"
#include <algorithm>
#include <chrono>

namespace trading {

namespace marketmath {

double mid(const MarketTick& t) {
    if (t.bid > 0.0 && t.ask > 0.0) return 0.5 * (t.bid + t.ask);
    if (t.bid > 0.0) return t.bid;
    return t.ask;
}

double spread(const MarketTick& t) {
    if (t.bid > 0.0 && t.ask > 0.0) return t.ask - t.bid;
    return 0.0;
}

double spreadPct(const MarketTick& t) {
    double m = mid(t);
    if (m <= 0.0) return 0.0;
    return spread(t) / m;
}

double bookImbalance(double bidQty, double askQty) {
    double total = bidQty + askQty;
    if (total <= 0.0) return 0.0;
    return (bidQty - askQty) / total;
}

double volumeImbalance(double buyVol, double sellVol) {
    double total = buyVol + sellVol;
    if (total <= 0.0) return 0.0;
    return (buyVol - sellVol) / total;
}

void derive(MarketTick& t) {
    t.mid = mid(t);
    t.spread = spread(t);
    t.bookImbalance = bookImbalance(t.bidSize, t.askSize);
    t.derived = true;
}

} // namespace marketmath

// ---------------------------------------------------------------------------
// MarketStateCache
// ---------------------------------------------------------------------------

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void MarketStateCache::update(const MarketTick& tick) {
    std::lock_guard<std::mutex> lock(mutex_);
    MarketTick t = tick;
    marketmath::derive(t);
    if (t.localTimestamp == 0) t.localTimestamp = nowNs();
    ticks_[t.symbol] = std::move(t);
}

void MarketStateCache::updateBook(const OrderBook& book) {
    std::lock_guard<std::mutex> lock(mutex_);
    books_[book.symbol] = book;
}

std::optional<MarketTick> MarketStateCache::latest(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ticks_.find(symbol);
    if (it == ticks_.end()) return std::nullopt;
    return it->second;
}

bool MarketStateCache::isStale(const std::string& symbol, uint64_t maxAgeNs,
                               uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ticks_.find(symbol);
    if (it == ticks_.end()) return true;  // no data => stale
    uint64_t age = now > it->second.localTimestamp
                       ? now - it->second.localTimestamp
                       : 0;
    return age > maxAgeNs;
}

} // namespace trading
