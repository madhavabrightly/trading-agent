// Unit tests for the risk engine (Phase 11). Safety-critical behavior:
// kill switch, position/exposure caps, daily loss, stale data, cooldown.
#include "trading/risk/RiskEngine.hpp"
#include <chrono>
#include <cstdio>
#include <cmath>

using namespace trading;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

int main() {
    RiskConfig cfg;
    cfg.maxPositionSize = 0.10;
    cfg.maxOrderSize = 0.05;
    cfg.maxDailyLoss = 0.02;
    cfg.maxDrawdown = 0.20;
    cfg.maxLeverage = 2.0;
    cfg.maxOpenPositions = 3;
    cfg.cooldownSeconds = 60.0;
    RiskEngine engine(cfg);

    RiskPortfolioView pf;
    pf.equity = 100'000.0;
    pf.startingEquityDay = 100'000.0;
    pf.peakEquity = 100'000.0;

    TradingSignal sig;
    sig.symbol = "BTC/USD";
    sig.direction = TradingSignal::Direction::LONG;
    sig.entry = 50'000.0;
    sig.confidence = 0.8;

    MarketSnapshot mkt;
    mkt.symbol = "BTC/USD";
    mkt.bid = 49'990.0;
    mkt.ask = 50'010.0;
    mkt.last = 50'000.0;
    mkt.stale = false;

    // 1. Healthy long approved and sized to 5% of equity.
    auto d1 = engine.evaluate(sig, pf, &mkt);
    CHECK(d1.approved, "clean long approved");
    if (d1.approved) {
        // 5% of 100k = 5000 notional / 50000 price = 0.1 BTC
        CHECK(std::fabs(d1.allowedQuantity - 0.1) < 1e-9,
              "order sized to maxOrderSize fraction");
    }

    // 2. Kill switch blocks everything.
    engine.killSwitch().engage("manual test");
    auto d2 = engine.evaluate(sig, pf, &mkt);
    CHECK(!d2.approved, "kill switch rejects");
    CHECK(d2.reason.find("kill switch") != std::string::npos,
          "kill switch reason surfaced");
    engine.killSwitch().release();
    auto d3 = engine.evaluate(sig, pf, &mkt);
    CHECK(d3.approved, "kill switch release restores trading");

    // 3. Flat signal rejected.
    auto flat = sig;
    flat.direction = TradingSignal::Direction::FLAT;
    auto d4 = engine.evaluate(flat, pf, &mkt);
    CHECK(!d4.approved, "flat signal rejected");

    // 4. Position cap: existing position above 10% blocks new entry.
    Position pos;
    pos.symbol = "BTC/USD";
    pos.quantity = 0.21;    // 0.21 * 50k = 10500 = 10.5% of 100k
    pos.currentPrice = 50'000.0;
    pf.positions = {pos};
    auto d5 = engine.evaluate(sig, pf, &mkt);
    CHECK(!d5.approved, "symbol exposure cap blocks");
    pf.positions.clear();

    // 5. Stale market data blocks.
    mkt.stale = true;
    auto d6 = engine.evaluate(sig, pf, &mkt);
    CHECK(!d6.approved, "stale market data blocks");
    CHECK(d6.reason.find("stale") != std::string::npos, "stale reason surfaced");
    mkt.stale = false;

    // 6. Wide spread blocks.
    auto wide = mkt;
    wide.bid = 45'000.0;  // 10% spread
    wide.ask = 55'000.0;
    auto d7 = engine.evaluate(sig, pf, &wide);
    CHECK(!d7.approved, "wide spread blocks");

    // 7. Cooldown after an order placement.
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    engine.onOrderPlaced("BTC/USD", now);
    auto d8 = engine.evaluate(sig, pf, &mkt);
    CHECK(!d8.approved, "cooldown blocks immediate duplicate");

    // 8. Max open positions.
    RiskConfig cfg2 = cfg;
    cfg2.maxOpenPositions = 2;
    RiskEngine engine2(cfg2);
    RiskPortfolioView pf2 = pf;
    Position a, b;
    a.symbol = "ETH/USD"; a.quantity = 1.0; a.currentPrice = 3000.0;
    b.symbol = "SOL/USD"; b.quantity = 10.0; b.currentPrice = 150.0;
    pf2.positions = {a, b};
    auto d9 = engine2.evaluate(sig, pf2, &mkt);
    CHECK(!d9.approved, "max open positions blocks new entry");

    // 9. Daily loss limit.
    RiskPortfolioView losing = pf;
    losing.equity = 97'000.0;  // -3% day
    losing.startingEquityDay = 100'000.0;
    auto d10 = engine.evaluate(sig, losing, &mkt);
    CHECK(!d10.approved, "daily loss limit blocks");
    CHECK(d10.reason.find("daily loss") != std::string::npos, "daily loss reason");

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
