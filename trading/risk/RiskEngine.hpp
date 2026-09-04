#pragma once

// trading/risk/RiskEngine.hpp — the safety gate every order must pass through
// (spec §19, §56). Signals become orders ONLY after the risk engine approves.
// Implements the full checklist: max position/order size, daily loss,
// drawdown, leverage, open positions, exposure, correlation, spread/slippage,
// market-hours, duplicate protection, cooldown, kill switch.

#include "trading/core/Types.hpp"
#include "trading/signal/TradingSignal.hpp"
#include "trading/connectors/TradingTypes.hpp"
#include "trading/core/config/ConfigStore.hpp"
#include "trading/market/MarketData.hpp"
#include "trading/market/MarketTypes.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trading {

// The risk engine's view of the portfolio at evaluation time.
struct RiskPortfolioView {
    double equity = 0.0;
    double startingEquityDay = 0.0;   // equity at local midnight
    double realizedPnlDay = 0.0;
    double unrealizedPnl = 0.0;
    double usedMargin = 0.0;
    double peakEquity = 0.0;          // all-time peak (for drawdown)
    std::vector<Position> positions;
};

// Per-symbol state used for duplicate protection / cooldown / sizing.
class KillSwitch {
public:
    KillSwitch() = default;

    void engage(const std::string& reason) {
        engaged_.store(true);
        reason_ = reason;
    }
    void release() { engaged_.store(false); reason_.clear(); }
    bool engaged() const { return engaged_.load(); }
    std::string reason() const { return reason_; }

    // When engaged, no new orders may be placed; open orders are cancelled
    // only if explicitly configured.
    bool allowNewOrders() const { return !engaged_.load(); }

private:
    std::atomic<bool> engaged_{false};
    std::string reason_;
};

class RiskEngine {
public:
    explicit RiskEngine(const RiskConfig& config = RiskConfig{});

    void setConfig(const RiskConfig& cfg);
    const RiskConfig& config() const { return config_; }

    // Evaluates a signal -> RiskDecision. Pure, except for recording cooldown
    // + duplicate state on approval (via approveOrder).
    RiskDecision evaluate(const TradingSignal& signal,
                          const RiskPortfolioView& portfolio,
                          const MarketSnapshot* market);

    // Called after an order is placed; records cooldown + duplicate state.
    void onOrderPlaced(const std::string& symbol, uint64_t nowNs);

    // Daily reset (call at local midnight / session start).
    void resetDay(const RiskPortfolioView& portfolio);

    // Kill switch access.
    KillSwitch& killSwitch() { return kill_; }
    const KillSwitch& killSwitch() const { return kill_; }

    // Market-hours gate (when configured).
    static bool marketOpenNow(const std::string& timezone = "America/New_York");

private:
    RiskConfig config_;
    KillSwitch kill_;

    mutable std::mutex mutex_;
    std::map<std::string, uint64_t> lastOrderNs_;   // cooldown
    std::map<std::string, uint64_t> symbolCount_;   // duplicate protection
};

} // namespace trading
