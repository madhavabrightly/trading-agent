// Unit tests for SymbolResolver + market math (Phase 6).
#include "trading/symbols/SymbolResolver.hpp"
#include "trading/market/MarketData.hpp"
#include <cstdio>
#include <cmath>

using namespace trading;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    if (!((a) == (b))) { std::printf("[FAIL] %s (line %d): got '%s' want '%s'\n", msg, __LINE__, std::string(a).c_str(), std::string(b).c_str()); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

int main() {
    SymbolResolver res;

    // Heuristic normalization
    CHECK_EQ(res.normalizeHeuristic("BTCUSDT"), "BTC/USD", "BTCUSDT -> BTC/USD");
    CHECK_EQ(res.normalizeHeuristic("BTC-USD"), "BTC/USD", "BTC-USD -> BTC/USD");
    CHECK_EQ(res.normalizeHeuristic("XBTUSD"), "BTC/USD", "XBTUSD -> BTC/USD");
    CHECK_EQ(res.normalizeHeuristic("XAUUSD"), "XAU/USD", "XAUUSD -> XAU/USD");
    CHECK_EQ(res.normalizeHeuristic("GOLD"), "XAU/USD", "GOLD -> XAU/USD");
    CHECK_EQ(res.normalizeHeuristic("XAG/USD"), "XAG/USD", "already canonical passes through");
    CHECK_EQ(res.normalizeHeuristic("EURUSD"), "EUR/USD", "EURUSD -> EUR/USD");
    CHECK_EQ(res.normalizeHeuristic("AAPL"), "AAPL/USD", "stock ticker -> TICKER/USD");
    CHECK_EQ(res.normalizeHeuristic("eth_usdt"), "ETH/USD", "lowercase + underscore normalized");

    // Mapped aliases override heuristics
    SymbolResolver mapped;
    mapped.addAlias({"binance", "BTCUSDT", "BTC/USD", "crypto"});
    mapped.addAlias({"binance", "ETHUSDT", "ETH/USD", "crypto"});
    mapped.addAlias({"alpaca", "BTCUSD", "BTC/USD", "crypto"});
    auto c = mapped.canonical("binance", "BTCUSDT");
    CHECK(c.has_value() && *c == "BTC/USD", "mapped canonical lookup");
    auto n = mapped.native("alpaca", "BTC/USD");
    CHECK(n.has_value() && *n == "BTCUSD", "native reverse lookup");
    CHECK(!mapped.canonical("kraken", "123!@#").has_value(),
          "implausible symbol -> nullopt");
    auto h = mapped.canonical("somex", "BTCUSDT");
    CHECK(h.has_value() && *h == "BTC/USD", "unmapped falls back to heuristic");

    // Market math
    MarketTick t;
    t.symbol = "BTC/USD";
    t.bid = 100.0; t.ask = 101.0;
    t.bidSize = 5.0; t.askSize = 1.0;
    CHECK(std::fabs(marketmath::mid(t) - 100.5) < 1e-9, "mid = avg");
    CHECK(std::fabs(marketmath::spread(t) - 1.0) < 1e-9, "spread = ask-bid");
    CHECK(std::fabs(marketmath::spreadPct(t) - 1.0/100.5) < 1e-9, "spread pct");
    CHECK(std::fabs(marketmath::bookImbalance(5.0, 1.0) - (4.0/6.0)) < 1e-9,
          "book imbalance sign + magnitude");
    CHECK(std::fabs(marketmath::bookImbalance(0, 0)) < 1e-9, "zero imbalance on no size");

    MarketStateCache cache;
    t.localTimestamp = 1'000'000'000ULL;  // arbitrary ns
    cache.update(t);
    auto latest = cache.latest("BTC/USD");
    CHECK(latest.has_value(), "cache stores latest tick");
    if (latest) {
        CHECK(std::fabs(latest->mid - 100.5) < 1e-9, "cache derives mid");
        CHECK(latest->derived, "cache marks derived");
    }
    CHECK(cache.isStale("BTC/USD", 500ULL, 1'000'001'000ULL), "stale when age exceeds maxAge");
    CHECK(!cache.isStale("BTC/USD", 500ULL, 1'000'000'100ULL), "fresh within maxAge");
    CHECK(cache.isStale("UNKNOWN", 500ULL, 1'000'000'000ULL), "no data is stale");

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
