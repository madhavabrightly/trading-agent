#pragma once

// trading/market/MarketTypes.hpp — normalized market data structures (spec §9).
// Every connector translates its native feed into these canonical types.

#include <cstdint>
#include <string>
#include <vector>

namespace trading {

// A snapshot used for symbol validation / order pre-checks (spec §39).
struct MarketSnapshot {
    std::string symbol;          // canonical
    double bid = 0.0;
    double ask = 0.0;
    double last = 0.0;
    double vwap = 0.0;
    double spread = 0.0;
    bool marketOpen = true;
    uint64_t timestamp = 0;      // ns epoch (local receive)
    bool stale = false;          // set when older than the staleness window
};

// Canonical tick — normalized across exchanges.
struct MarketTick {
    std::string exchange;   // e.g. "alpaca"
    std::string symbol;     // canonical, e.g. "BTC/USD" or "AAPL/USD"

    double bid = 0.0;
    double ask = 0.0;
    double last = 0.0;

    double bidSize = 0.0;
    double askSize = 0.0;

    double volume = 0.0;

    uint64_t exchangeTimestamp = 0;  // ns epoch (source clock)
    uint64_t localTimestamp = 0;     // ns epoch (receive clock)

    // --- derived (filled by the normalizer, not the connector) ---
    bool derived = false;
    double spread = 0.0;
    double mid = 0.0;
    double vwap = 0.0;
    double volatility = 0.0;       // rolling estimate
    double momentum = 0.0;         // signed
    double bookImbalance = 0.0;    // [-1, 1]
    double volumeImbalance = 0.0;  // [-1, 1]
};

struct Trade {
    std::string exchange;
    std::string symbol;
    double price = 0.0;
    double quantity = 0.0;
    bool buyerIsMaker = false;     // when known
    uint64_t timestamp = 0;        // ns epoch
};

struct OrderBookLevel {
    double price = 0.0;
    double quantity = 0.0;
};

struct OrderBook {
    std::string exchange;
    std::string symbol;
    std::vector<OrderBookLevel> bids;  // best first
    std::vector<OrderBookLevel> asks;  // best first
    uint64_t timestamp = 0;            // ns epoch
};

struct OhlcvBar {
    std::string exchange;
    std::string symbol;
    uint64_t startTime = 0;   // ns epoch
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    bool closed = false;
};

} // namespace trading
