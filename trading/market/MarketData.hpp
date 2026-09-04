#pragma once

// trading/market/MarketData.hpp — derived market math (spec §9) + the
// staleness guard (spec §34) used before any trading decision.

#include "trading/market/MarketTypes.hpp"
#include <string>
#include <map>
#include <mutex>
#include <optional>

namespace trading {

// Pure functions — no state.
namespace marketmath {

// Spread in quote units and as a fraction of mid.
double spread(const MarketTick& t);
double spreadPct(const MarketTick& t);
double mid(const MarketTick& t);

// Signed order-book imbalance in [-1, 1] (positive = bid pressure).
double bookImbalance(double bidQty, double askQty);

// Signed volume imbalance in [-1, 1].
double volumeImbalance(double buyVol, double sellVol);

// Fill derived fields on a tick (mid, spread, imbalances). Deterministic.
void derive(MarketTick& t);

} // namespace marketmath

// Tracks the latest canonical-symbol state and answers staleness queries.
// The risk engine / strategies ask this before acting on data (spec §34).
class MarketStateCache {
public:
    MarketStateCache() = default;

    void update(const MarketTick& tick);
    void updateBook(const OrderBook& book);

    std::optional<MarketTick> latest(const std::string& symbol) const;
    // True when the newest tick for symbol is older than maxAgeNs.
    bool isStale(const std::string& symbol, uint64_t maxAgeNs,
                 uint64_t nowNs) const;

    void setStaleAfterNs(uint64_t ns) { staleAfterNs_ = ns; }

private:
    mutable std::mutex mutex_;
    std::map<std::string, MarketTick> ticks_;
    std::map<std::string, OrderBook> books_;
    uint64_t staleAfterNs_ = 2'000'000'000ULL;  // 2s default
};

} // namespace trading
