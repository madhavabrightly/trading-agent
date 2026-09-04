#pragma once

// trading/connectors/ITradingConnector.hpp — every broker/exchange connector
// implements this interface (spec §5). The system only talks to connectors
// through it, so a paper simulator can stand in for a live broker and the rest
// of the pipeline is unchanged.

#include "trading/connectors/TradingTypes.hpp"
#include "trading/market/MarketTypes.hpp"
#include <functional>
#include <memory>
#include <string>

namespace trading {

class ITradingConnector {
public:
    virtual ~ITradingConnector() = default;

    // --- lifecycle ---
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool authenticate() = 0;
    virtual bool isAuthenticated() const = 0;

    // --- identity ---
    virtual std::string name() const = 0;        // "alpaca", "binance", ...
    virtual EnvMode environment() const = 0;     // never silently changed

    // --- account / positions / orders ---
    virtual AccountInfo getAccount() = 0;
    virtual std::vector<Position> getPositions() = 0;
    virtual std::vector<Order> getOpenOrders() = 0;

    virtual OrderResult placeOrder(const OrderRequest& request) = 0;
    virtual CancelResult cancelOrder(const std::string& orderId) = 0;
    virtual OrderResult modifyOrder(const std::string& orderId,
                                    const OrderModification& mod) = 0;

    // --- market data ---
    virtual MarketSnapshot getMarketSnapshot(const std::string& symbol) = 0;
    virtual void subscribeMarketData(const SubscriptionRequest& request) = 0;
    virtual void unsubscribeMarketData(const std::string& symbol) = 0;

    // Market data callbacks (set by the connection manager).
    using TickCallback = std::function<void(const MarketTick&)>;
    virtual void setTickCallback(TickCallback cb) = 0;

    // --- capabilities / validation (spec §38, §39) ---
    virtual const ConnectorCapabilities& capabilities() const = 0;

    // Full API-level health check (spec §30): auth, account, market stream,
    // order endpoint — never "connected" on TCP alone.
    virtual ConnectorHealth testConnection() = 0;
};

using TradingConnectorPtr = std::shared_ptr<ITradingConnector>;

} // namespace trading
