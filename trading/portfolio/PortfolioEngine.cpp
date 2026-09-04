#include "trading/portfolio/PortfolioEngine.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>

namespace trading {

namespace {

double absOr0(double v) { return std::fabs(v) < 1e-12 ? 0.0 : std::fabs(v); }

std::string canonicalKey(const Order& o) {
    // Broker order id is the durable cross-reference.
    return o.orderId.empty() ? o.clientOrderId : o.orderId;
}

} // namespace

void PortfolioEngine::update(const std::string& connector,
                             const AccountInfo& account,
                             const std::vector<Position>& positions) {
    std::lock_guard<std::mutex> lock(mutex_);
    PortfolioSnapshot& s = snapshots_[connector];
    s.connector = connector;
    s.equity = account.equity;
    s.cash = account.cash;
    s.buyingPower = account.buyingPower;
    s.usedMargin = account.initialMargin;
    s.unrealizedPnl = account.unrealizedPnl;
    s.realizedPnl = account.realizedPnl;
    s.timestamp = 0;

    // Track local positions + orders for reconciliation.
    localPositions_.clear();
    for (const auto& p : positions) {
        if (absOr0(p.quantity) > 1e-12) localPositions_[p.symbol] = p;
    }

    // Exposure + leverage.
    double exposure = 0.0;
    for (const auto& p : positions) {
        double price = p.currentPrice > 0.0 ? p.currentPrice : p.avgEntryPrice;
        exposure += absOr0(p.quantity) * price;
    }
    s.exposure = exposure;
    s.leverage = s.equity > 0.0 ? exposure / s.equity : 0.0;

    // Drawdown from all-time peak.
    if (s.equity > peakEquity_) peakEquity_ = s.equity;
    if (peakEquity_ > 0.0)
        s.drawdown = (peakEquity_ - s.equity) / peakEquity_;

    // Trade stats.
    s.winRate = (wins_ + losses_) > 0 ? static_cast<double>(wins_) /
                                            static_cast<double>(wins_ + losses_)
                                      : 0.0;
    s.profitFactor = grossLoss_ > 0.0 ? grossProfit_ / grossLoss_ : 0.0;
    s.positions = positions;
}

void PortfolioEngine::recordClosedTrade(double realizedPnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (realizedPnl >= 0.0) {
        wins_++;
        grossProfit_ += realizedPnl;
    } else {
        losses_++;
        grossLoss_ += std::fabs(realizedPnl);
    }
}

void PortfolioEngine::recordEquity(double equity, uint64_t nowNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (equity > peakEquity_) peakEquity_ = equity;
    // Sharpe: keep a small ring of daily returns.
    (void)nowNs;
}

PortfolioSnapshot PortfolioEngine::snapshot(const std::string& connector) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = snapshots_.find(connector);
    if (it == snapshots_.end()) return PortfolioSnapshot{};
    return it->second;
}

double PortfolioEngine::peakEquity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peakEquity_;
}

std::vector<PortfolioEngine::ReconcileIssue> PortfolioEngine::reconcile(
    const std::vector<Order>& localOrders,
    const std::vector<Order>& brokerOrders,
    const std::vector<Position>& localPositions,
    const std::vector<Position>& brokerPositions) {
    std::vector<ReconcileIssue> issues;

    // Build broker-side lookup by id.
    std::map<std::string, const Order*> brokerById;
    for (const auto& o : brokerOrders) brokerById[canonicalKey(o)] = &o;

    // Every local open order should exist on the broker in a consistent state.
    for (const auto& lo : localOrders) {
        auto it = brokerById.find(canonicalKey(lo));
        if (it == brokerById.end()) {
            issues.push_back({"order", "local order missing on broker",
                              canonicalKey(lo), ""});
            continue;
        }
        const Order& bo = *it->second;
        if (lo.status == OrderStatus::FILLED &&
            bo.status != OrderStatus::FILLED) {
            issues.push_back({"order", "local FILLED but broker reports " +
                                           std::string(toString(bo.status)),
                              canonicalKey(lo), canonicalKey(bo)});
        }
        double qtyDiff = std::fabs(lo.filledQuantity - bo.filledQuantity);
        if (qtyDiff > 1e-9) {
            issues.push_back({"order", "filled quantity mismatch", canonicalKey(lo),
                              canonicalKey(bo)});
        }
    }

    // Broker orders that are locally unknown (missed ack).
    for (const auto& bo : brokerOrders) {
        bool found = false;
        for (const auto& lo : localOrders)
            if (canonicalKey(lo) == canonicalKey(bo)) found = true;
        if (!found && (bo.status == OrderStatus::ACKNOWLEDGED ||
                       bo.status == OrderStatus::PARTIALLY_FILLED ||
                       bo.status == OrderStatus::FILLED)) {
            issues.push_back({"order", "broker order unknown locally", "",
                              canonicalKey(bo)});
        }
    }

    // Position reconciliation.
    std::map<std::string, const Position*> brokerPos;
    for (const auto& p : brokerPositions)
        if (absOr0(p.quantity) > 1e-12) brokerPos[p.symbol] = &p;
    std::map<std::string, const Position*> localPos;
    for (const auto& p : localPositions)
        if (absOr0(p.quantity) > 1e-12) localPos[p.symbol] = &p;

    for (const auto& [sym, lp] : localPos) {
        auto it = brokerPos.find(sym);
        if (it == brokerPos.end()) {
            issues.push_back({"position", "local position missing on broker", sym,
                              ""});
        } else {
            const Position& bp = *it->second;
            if (std::fabs(lp->quantity - bp.quantity) > 1e-9) {
                issues.push_back({"position", "quantity mismatch", sym, sym});
            }
        }
    }
    for (const auto& [sym, bp] : brokerPos) {
        if (localPos.find(sym) == localPos.end())
            issues.push_back({"position", "broker position unknown locally", "",
                              sym});
    }
    return issues;
}

void PortfolioEngine::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.clear();
    localPositions_.clear();
    localOrders_.clear();
    peakEquity_ = 0.0;
    wins_ = losses_ = 0;
    grossProfit_ = grossLoss_ = 0.0;
}

} // namespace trading
