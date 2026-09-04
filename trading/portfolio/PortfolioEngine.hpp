#pragma once

// trading/portfolio/PortfolioEngine.hpp — normalized portfolio tracking across
// brokers (spec §23): balance/equity/margin/P&L/exposure/leverage/drawdown/
// win-rate/profit-factor. Reconciliation (spec §22) compares local vs broker
// state and reports mismatches — never auto-fixes them.

#include "trading/connectors/TradingTypes.hpp"
#include "trading/core/config/ConfigStore.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trading {

struct PortfolioSnapshot {
    std::string connector;
    double equity = 0.0;
    double cash = 0.0;
    double buyingPower = 0.0;
    double usedMargin = 0.0;
    double unrealizedPnl = 0.0;
    double realizedPnl = 0.0;
    double exposure = 0.0;
    double leverage = 0.0;
    double drawdown = 0.0;         // from peak
    double winRate = 0.0;          // 0..1
    double profitFactor = 0.0;
    double sharpe = 0.0;
    uint64_t timestamp = 0;
    std::vector<Position> positions;
};

class PortfolioEngine {
public:
    PortfolioEngine() = default;

    // Updates from a connector's view of account + positions.
    void update(const std::string& connector, const AccountInfo& account,
                const std::vector<Position>& positions);

    // Records a closed-trade result for win-rate / profit-factor.
    void recordClosedTrade(double realizedPnl);

    // Rolling daily P&L series for Sharpe (call once per snapshot).
    void recordEquity(double equity, uint64_t nowNs);

    PortfolioSnapshot snapshot(const std::string& connector) const;

    // Peak equity across all connectors (drawdown basis).
    double peakEquity() const;

    // --- Reconciliation (spec §22) ---
    struct ReconcileIssue {
        std::string kind;      // "order" | "position"
        std::string detail;
        std::string localId;
        std::string brokerId;
    };

    // Compares locally-tracked state against the broker's reported state.
    // Never modifies either side; returns the list of mismatches.
    static std::vector<ReconcileIssue> reconcile(
        const std::vector<Order>& localOrders,
        const std::vector<Order>& brokerOrders,
        const std::vector<Position>& localPositions,
        const std::vector<Position>& brokerPositions);

    void reset();

private:
    mutable std::mutex mutex_;
    std::map<std::string, PortfolioSnapshot> snapshots_;
    std::map<std::string, Position> localPositions_;
    std::vector<Order> localOrders_;
    double peakEquity_ = 0.0;
    double peakDrawdown_ = 0.0;

    // Closed-trade stats.
    uint64_t wins_ = 0;
    uint64_t losses_ = 0;
    double grossProfit_ = 0.0;
    double grossLoss_ = 0.0;
};

} // namespace trading
