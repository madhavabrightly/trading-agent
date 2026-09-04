#pragma once

// trading/execution/PaperSimulator.hpp — paper/simulation executor shaped as
// an ITradingConnector (spec §24). SIMULATION/PAPER/BACKTEST share the same
// signal/risk/order/position/portfolio interfaces; only the execution
// connector differs. Fills against injected market ticks with configurable
// latency + slippage + partial fills.

#include "trading/connectors/ITradingConnector.hpp"
#include "trading/execution/OrderStateMachine.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace trading {

class PaperSimulator : public ITradingConnector {
public:
    struct Settings {
        std::string label = "paper";
        double slippageBps = 1.0;        // 1 bp default
        double fillDelayMs = 25.0;       // simulated round-trip latency
        double partialFillProbability = 0.0;
        double startingCash = 100'000.0;
    };

    explicit PaperSimulator(Settings s);
    PaperSimulator() : PaperSimulator(Settings{}) {}
    ~PaperSimulator() override;

    // --- ITradingConnector ---
    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool authenticate() override { return true; }
    bool isAuthenticated() const override { return connected_.load(); }

    std::string name() const override { return "paper"; }
    EnvMode environment() const override { return EnvMode::SIMULATION; }

    AccountInfo getAccount() override;
    std::vector<Position> getPositions() override;
    std::vector<Order> getOpenOrders() override;

    OrderResult placeOrder(const OrderRequest& request) override;
    CancelResult cancelOrder(const std::string& orderId) override;
    OrderResult modifyOrder(const std::string& orderId,
                            const OrderModification& mod) override;

    MarketSnapshot getMarketSnapshot(const std::string& symbol) override;
    void subscribeMarketData(const SubscriptionRequest&) override {}
    void unsubscribeMarketData(const std::string&) override {}
    void setTickCallback(TickCallback cb) override { tickCb_ = std::move(cb); }

    const ConnectorCapabilities& capabilities() const override {
        return caps_;
    }
    ConnectorHealth testConnection() override;

    // Feed market ticks into the simulator (from the market engine or a
    // backtest replay). Drives fills + P&L.
    void onMarketTick(const MarketTick& tick);

    // For tests: advance time by injecting a tick with explicit local ts.
    void injectFill(const std::string& orderId, double price, double qty);

    Settings settings() const { return settings_; }

private:
    // Books a fill against an open order (state machine enforced).
    bool applyFill(const std::string& orderId, double price, double qty,
                   OrderStatus afterFill);
    Position* positionFor(const std::string& symbol);

    Settings settings_;
    ConnectorCapabilities caps_;
    std::atomic<bool> connected_{false};

    mutable std::mutex mutex_;
    double cash_ = 0.0;
    double equity_ = 0.0;
    uint64_t seq_ = 0;
    std::map<std::string, Order> orders_;        // by clientOrderId
    std::map<std::string, OrderStateMachine> machines_;
    std::map<std::string, Position> positions_;  // by symbol
    std::map<std::string, MarketTick> lastTicks_;
    TickCallback tickCb_;
};

} // namespace trading
