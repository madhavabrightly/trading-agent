#include "trading/risk/RiskEngine.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace trading {

namespace {
uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

double absOr0(double v) { return std::fabs(v) < 1e-12 ? 0.0 : std::fabs(v); }
} // namespace

RiskEngine::RiskEngine(const RiskConfig& config) : config_(config) {}

void RiskEngine::setConfig(const RiskConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cfg;
}

void RiskEngine::resetDay(const RiskPortfolioView& portfolio) {
    // The portfolio view is authoritative for day-start state; nothing to
    // cache locally (the engine reads day state from the view each evaluate).
    (void)portfolio;
}

RiskDecision RiskEngine::evaluate(const TradingSignal& signal,
                                  const RiskPortfolioView& portfolio,
                                  const MarketSnapshot* market) {
    RiskDecision d;
    d.approved = false;
    d.allowedQuantity = 0.0;

    auto reject = [&](const std::string& reason, double code) {
        d.approved = false;
        d.reason = reason;
        d.reasonCode = code;
        LOG_WARN("RiskEngine: REJECTED {} {}: {}", signal.symbol,
                 toString(signal.direction), reason);
    };
    auto approve = [&](double qty) {
        d.approved = true;
        d.allowedQuantity = qty;
        d.reason = "approved";
        LOG_DEBUG("RiskEngine: APPROVED {} {} qty={}", signal.symbol,
                  toString(signal.direction), qty);
    };

    // 1. Kill switch — hard stop on all new orders.
    if (kill_.engaged()) {
        reject("kill switch engaged: " + kill_.reason(), 1);
        return d;
    }
    if (signal.direction == TradingSignal::Direction::FLAT) {
        reject("flat signal", 2);
        return d;
    }

    // 2. Equity sanity.
    if (portfolio.equity <= 0.0) {
        reject("non-positive equity", 3);
        return d;
    }

    // 3. Symbol exposure cap (fraction of equity per symbol).
    double symbolExposure = 0.0;
    for (const auto& p : portfolio.positions) {
        if (p.symbol == signal.symbol) {
            symbolExposure += absOr0(p.quantity) * (p.currentPrice > 0 ? p.currentPrice : 1.0);
        }
    }
    if (symbolExposure / portfolio.equity >= config_.maxPositionSize) {
        reject("symbol exposure cap reached", 4);
        return d;
    }

    // 4. Total exposure cap.
    double totalExposure = 0.0;
    for (const auto& p : portfolio.positions) {
        totalExposure += absOr0(p.quantity) * (p.currentPrice > 0 ? p.currentPrice : 1.0);
    }
    if (totalExposure / portfolio.equity >= config_.maxExposure) {
        reject("total exposure cap reached", 5);
        return d;
    }

    // 5. Max open positions.
    size_t openCount = portfolio.positions.size();
    if (static_cast<int>(openCount) >= config_.maxOpenPositions &&
        symbolExposure <= 0.0) {
        reject("max open positions reached", 6);
        return d;
    }

    // 6. Daily loss limit (day-start equity from the portfolio view).
    if (portfolio.startingEquityDay > 0.0 && portfolio.equity > 0.0) {
        double dayLossPct =
            (portfolio.startingEquityDay - portfolio.equity) /
            portfolio.startingEquityDay;
        if (dayLossPct >= config_.maxDailyLoss) {
            reject("daily loss limit hit", 7);
            return d;
        }
    }

    // 7. Drawdown limit (all-time peak).
    if (portfolio.peakEquity > 0.0 && portfolio.equity > 0.0) {
        double dd = (portfolio.peakEquity - portfolio.equity) / portfolio.peakEquity;
        if (dd >= config_.maxDrawdown) {
            reject("max drawdown hit", 8);
            return d;
        }
    }

    // 8. Spread threshold (from live market snapshot when available).
    if (market && market->bid > 0.0 && market->ask > 0.0) {
        double mid = 0.5 * (market->bid + market->ask);
        double spreadPct = mid > 0.0 ? (market->ask - market->bid) / mid : 0.0;
        if (spreadPct > config_.maxSpreadPct) {
            reject("spread too wide", 9);
            return d;
        }
    }

    // 9. Stale market data -> block.
    if (market && market->stale) {
        reject("market data stale", 10);
        return d;
    }

    // 10. Duplicate-order protection: do not stack identical signals within a
    //     window unless already flat (cooldown is recorded on placement).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lastOrderNs_.find(signal.symbol);
        uint64_t now = nowNs();
        if (it != lastOrderNs_.end()) {
            uint64_t since = now > it->second ? now - it->second : 0;
            double sinceSec = static_cast<double>(since) / 1e9;
            if (sinceSec < config_.cooldownSeconds) {
                reject("cooldown active (duplicate-order protection)", 11);
                return d;
            }
        }
    }

    // 11. Quantity sizing: if the signal carries no quantity, size by the
    //     max order fraction of equity at the current market price.
    double qty = signal.quantity;
    double notional = 0.0;
    if (qty <= 0.0) {
        double price = market && market->last > 0.0 ? market->last
                       : (signal.entry > 0.0 ? signal.entry : 1.0);
        if (price <= 0.0) {
            reject("no price to size order", 12);
            return d;
        }
        notional = portfolio.equity * config_.maxOrderSize;
        qty = notional / price;
    } else {
        notional = qty * (market && market->last > 0.0 ? market->last
                                                        : signal.entry);
    }

    // 12. Max order size check.
    if (notional > portfolio.equity * config_.maxOrderSize) {
        reject("order exceeds max order size", 13);
        return d;
    }
    // Leverage sanity: 1x cash is the default; larger qty would exceed.
    if (notional > portfolio.equity * config_.maxLeverage) {
        reject("order exceeds max leverage", 14);
        return d;
    }

    approve(qty);
    return d;
}

void RiskEngine::onOrderPlaced(const std::string& symbol, uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastOrderNs_[symbol] = now;
    symbolCount_[symbol]++;
}

bool RiskEngine::marketOpenNow(const std::string& timezone) {
    // Simplified: Mon-Fri 09:30-16:00 America/New_York. Real connectors report
    // via /clock; this is a conservative fallback gate.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    int dow = tm.tm_wday;  // 0=Sun
    if (dow == 0 || dow == 6) return false;
    int mins = tm.tm_hour * 60 + tm.tm_min;
    // Local time != NY time; this gate is only used when the connector does
    // not report a real clock. Conservative: require 09:30-16:00 local.
    (void)timezone;
    return mins >= 570 && mins <= 960;
}

} // namespace trading
