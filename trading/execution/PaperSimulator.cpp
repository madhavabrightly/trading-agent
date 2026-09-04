#include "trading/execution/PaperSimulator.hpp"
#include "logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace trading {

namespace {

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string shortId(uint64_t n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "paper-%08llx", static_cast<unsigned long long>(n));
    return buf;
}

} // namespace

PaperSimulator::PaperSimulator(Settings s)
    : settings_(std::move(s)), cash_(settings_.startingCash),
      equity_(settings_.startingCash) {
    caps_.spot = true;
    caps_.shorting = true;
    caps_.maxLeverage = 1.0;
}

PaperSimulator::~PaperSimulator() = default;

bool PaperSimulator::connect() {
    connected_.store(true);
    return true;
}

void PaperSimulator::disconnect() {
    connected_.store(false);
}

bool PaperSimulator::isConnected() const {
    return connected_.load();
}

AccountInfo PaperSimulator::getAccount() {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountInfo a;
    a.connector = "paper";
    a.accountId = "sim-account";
    a.env = EnvMode::SIMULATION;
    a.equity = equity_;
    a.cash = cash_;
    a.buyingPower = cash_;
    a.currency = "USD";
    return a;
}

Position* PaperSimulator::positionFor(const std::string& symbol) {
    auto it = positions_.find(symbol);
    return it == positions_.end() ? nullptr : &it->second;
}

std::vector<Position> PaperSimulator::getPositions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Position> out;
    for (const auto& [sym, p] : positions_) {
        if (std::fabs(p.quantity) > 1e-12) out.push_back(p);
    }
    return out;
}

std::vector<Order> PaperSimulator::getOpenOrders() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Order> out;
    for (const auto& [id, o] : orders_) {
        if (o.status == OrderStatus::ACKNOWLEDGED ||
            o.status == OrderStatus::PARTIALLY_FILLED ||
            o.status == OrderStatus::SUBMITTED)
            out.push_back(o);
    }
    return out;
}

OrderResult PaperSimulator::placeOrder(const OrderRequest& request) {
    OrderResult res;
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = ++seq_;
    Order o;
    o.connector = "paper";
    o.orderId = shortId(id);
    o.clientOrderId = request.clientOrderId.empty()
                          ? std::string("tc-") + o.orderId
                          : request.clientOrderId;
    o.symbol = request.symbol;
    o.side = request.side;
    o.type = request.type;
    o.requestedQuantity = request.quantity;
    o.limitPrice = request.limitPrice;
    o.stopPrice = request.stopPrice;
    o.tif = request.tif;
    o.status = OrderStatus::CREATED;
    o.createdAt = nowNs();
    o.updatedAt = o.createdAt;

    OrderStateMachine sm;
    std::string err;
    sm.transition(OrderStatus::VALIDATING, err);
    sm.transition(OrderStatus::SUBMITTED, err);
    sm.transition(OrderStatus::ACKNOWLEDGED, err);
    o.status = OrderStatus::ACKNOWLEDGED;
    o.statusMessage = "acknowledged by paper engine";

    orders_[o.clientOrderId] = o;
    machines_[o.clientOrderId] = sm;
    res.ok = true;
    res.order = o;
    LOG_INFO("PaperSimulator: order {} {} {} qty={} acknowledged ({})",
             o.symbol, toString(o.side), toString(o.type), o.requestedQuantity,
             o.orderId);
    return res;
}

CancelResult PaperSimulator::cancelOrder(const std::string& orderId) {
    CancelResult res;
    res.orderId = orderId;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, o] : orders_) {
        if (o.orderId == orderId && !o.statusMessage.empty()) {
            if (o.status == OrderStatus::ACKNOWLEDGED ||
                o.status == OrderStatus::PARTIALLY_FILLED) {
                o.status = OrderStatus::CANCELLED;
                o.statusMessage = "cancelled by paper engine";
                res.ok = true;
                res.status = OrderStatus::CANCELLED;
                return res;
            }
        }
    }
    res.error = "order not found or not cancellable";
    return res;
}

OrderResult PaperSimulator::modifyOrder(const std::string& orderId,
                                        const OrderModification& mod) {
    OrderResult res;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, o] : orders_) {
        if (o.orderId == orderId) {
            if (mod.quantity > 0) o.requestedQuantity = mod.quantity;
            if (mod.limitPrice > 0) o.limitPrice = mod.limitPrice;
            if (mod.stopPrice > 0) o.stopPrice = mod.stopPrice;
            res.ok = true;
            res.order = o;
            return res;
        }
    }
    res.error = "order not found";
    return res;
}

MarketSnapshot PaperSimulator::getMarketSnapshot(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    MarketSnapshot s;
    s.symbol = symbol;
    auto it = lastTicks_.find(symbol);
    if (it != lastTicks_.end()) {
        s.bid = it->second.bid;
        s.ask = it->second.ask;
        s.last = it->second.last;
        s.spread = it->second.ask - it->second.bid;
        s.timestamp = it->second.localTimestamp;
    }
    return s;
}

void PaperSimulator::onMarketTick(const MarketTick& tick) {
    // Determine fill prices from the tick.
    double buyPrice = tick.ask > 0.0 ? tick.ask : tick.last;
    double sellPrice = tick.bid > 0.0 ? tick.bid : tick.last;
    if (buyPrice <= 0.0 || sellPrice <= 0.0) return;

    std::vector<std::pair<std::string, double>> fills;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastTicks_[tick.symbol] = tick;

        // Update position mark-to-market + equity.
        equity_ = cash_;
        for (auto& [sym, p] : positions_) {
            auto it = lastTicks_.find(sym);
            if (it != lastTicks_.end() && it->second.last > 0.0) {
                p.currentPrice = it->second.last;
                p.unrealizedPnl = p.quantity * (p.currentPrice - p.avgEntryPrice);
            }
            equity_ += p.quantity * p.currentPrice;
        }

        // Fill any ACKNOWLEDGED market/limit orders for this symbol.
        for (auto& [clientId, o] : orders_) {
            if (o.symbol != tick.symbol) continue;
            if (o.status != OrderStatus::ACKNOWLEDGED &&
                o.status != OrderStatus::PARTIALLY_FILLED)
                continue;
            if (o.type == OrderType::LIMIT) {
                if (o.side == Side::BUY && tick.ask > o.limitPrice) continue;
                if (o.side == Side::SELL && tick.bid < o.limitPrice) continue;
            }
            double price = o.side == Side::BUY ? buyPrice : sellPrice;
            // Apply configured slippage.
            double slip = price * settings_.slippageBps / 10000.0;
            price = o.side == Side::BUY ? price + slip : price - slip;
            double remaining = o.requestedQuantity - o.filledQuantity;
            if (remaining <= 0.0) continue;
            fills.push_back({clientId, price});  // key by clientOrderId
            break;  // one fill per symbol per tick (simple model)
        }
    }
    // Apply fills outside the lock (still single-threaded here).
    for (const auto& [clientId, price] : fills) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = orders_.find(clientId);
        if (it == orders_.end()) continue;
        auto& o = it->second;
        if (o.status != OrderStatus::ACKNOWLEDGED &&
            o.status != OrderStatus::PARTIALLY_FILLED)
            continue;
        double remaining = o.requestedQuantity - o.filledQuantity;
        applyFill(clientId, price, remaining, OrderStatus::FILLED);
    }
}

void PaperSimulator::injectFill(const std::string& orderId, double price,
                                double qty) {
    std::lock_guard<std::mutex> lock(mutex_);
    applyFill(orderId, price, qty, OrderStatus::FILLED);
}

bool PaperSimulator::applyFill(const std::string& orderId, double price,
                               double qty, OrderStatus afterFill) {
    auto it = orders_.find(orderId);
    if (it == orders_.end()) return false;
    auto& o = it->second;
    if (qty <= 0.0) return false;
    o.filledQuantity += qty;
    // Weighted average fill price.
    o.avgFillPrice = o.filledQuantity > 0.0
                         ? (o.avgFillPrice * (o.filledQuantity - qty) +
                            price * qty) /
                               o.filledQuantity
                         : price;
    // Update position + cash.
    double signedQty = o.side == Side::BUY ? qty : -qty;
    auto& p = positions_[o.symbol];
    if (std::fabs(p.quantity) < 1e-12) {
        p.connector = "paper";
        p.symbol = o.symbol;
        p.quantity = signedQty;
        p.avgEntryPrice = price;
    } else {
        // Average into the position (sign-aware).
        double oldQty = p.quantity;
        double newQty = oldQty + signedQty;
        if (std::fabs(newQty) < 1e-12) {
            positions_.erase(o.symbol);
        } else {
            // Only blend avg entry when adding to the same direction.
            if ((oldQty > 0 && signedQty > 0) || (oldQty < 0 && signedQty < 0)) {
                p.avgEntryPrice = (std::fabs(oldQty) * p.avgEntryPrice +
                                   std::fabs(signedQty) * price) /
                                  (std::fabs(oldQty) + std::fabs(signedQty));
            }
            p.quantity = newQty;
            p.avgEntryPrice = std::fabs(newQty) > 0 ? p.avgEntryPrice : 0.0;
        }
    }
    // Cash impact (simple: no fees modeled in starter).
    cash_ -= o.side == Side::BUY ? price * qty : -price * qty;
    o.status = afterFill;
    o.updatedAt = nowNs();
    o.statusMessage = afterFill == OrderStatus::FILLED
                          ? "filled by paper engine"
                          : "partially filled";
    LOG_INFO("PaperSimulator: fill {} {} qty={} @ {:.2f}", o.symbol,
             toString(o.side), qty, price);
    return true;
}

ConnectorHealth PaperSimulator::testConnection() {
    ConnectorHealth h;
    h.connected = true;
    h.authenticated = true;
    h.status = "PASS (paper)";
    return h;
}

} // namespace trading
