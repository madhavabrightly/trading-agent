#pragma once

// trading/connectors/TradingTypes.hpp — broker-facing types shared by every
// connector (spec §5, §8, §21, §23). Connectors map their native API models
// onto these; the rest of the system never sees broker-specific structs.

#include "trading/core/Types.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace trading {

// ---------------------------------------------------------------------------
// Account
// ---------------------------------------------------------------------------

struct AccountInfo {
    std::string connector;     // logical connector name
    std::string accountId;
    EnvMode env = EnvMode::SIMULATION;

    double equity = 0.0;
    double cash = 0.0;
    double buyingPower = 0.0;
    double initialMargin = 0.0;
    double maintenanceMargin = 0.0;
    double unrealizedPnl = 0.0;
    double realizedPnl = 0.0;

    std::string currency = "USD";
    bool tradingBlocked = false;
    std::string status;        // raw broker status text
};

// ---------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------

struct Position {
    std::string connector;
    std::string symbol;      // canonical
    double quantity = 0.0;   // signed: + long, - short
    double avgEntryPrice = 0.0;
    double currentPrice = 0.0;
    double unrealizedPnl = 0.0;
    double realizedPnl = 0.0;
    double leverage = 1.0;
    uint64_t updatedAt = 0;  // ns epoch
    std::string raw;         // opaque broker payload (trimmed), for reconciliation
};

// ---------------------------------------------------------------------------
// Orders
// ---------------------------------------------------------------------------

struct OrderRequest {
    std::string connector;
    std::string symbol;              // canonical
    Side side = Side::BUY;
    OrderType type = OrderType::MARKET;
    double quantity = 0.0;           // base units
    double limitPrice = 0.0;         // LIMIT / STOP_LIMIT
    double stopPrice = 0.0;          // STOP / STOP_LIMIT
    TimeInForce tif = TimeInForce::GTC;
    bool reduceOnly = false;
    bool postOnly = false;
    std::string clientOrderId;       // local dedup id
    std::string exchangeAccountId;   // when the broker needs it
};

struct Order {
    std::string connector;
    std::string orderId;             // broker id (empty until acknowledged)
    std::string clientOrderId;       // our id
    std::string symbol;
    Side side = Side::BUY;
    OrderType type = OrderType::MARKET;
    double requestedQuantity = 0.0;
    double filledQuantity = 0.0;
    double avgFillPrice = 0.0;
    double limitPrice = 0.0;
    double stopPrice = 0.0;
    TimeInForce tif = TimeInForce::GTC;
    bool reduceOnly = false;
    bool postOnly = false;
    OrderStatus status = OrderStatus::CREATED;
    std::string statusMessage;
    uint64_t createdAt = 0;          // ns epoch (local)
    uint64_t updatedAt = 0;          // ns epoch (local)
    std::string raw;                 // trimmed broker payload for reconciliation
};

struct OrderResult {
    bool ok = false;
    std::string error;               // human-readable, never contains secrets
    Order order;                     // populated on success
};

struct CancelResult {
    bool ok = false;
    std::string error;
    std::string orderId;
    OrderStatus status = OrderStatus::UNKNOWN;
};

struct OrderModification {
    double quantity = 0.0;           // 0 = unchanged
    double limitPrice = 0.0;         // 0 = unchanged
    double stopPrice = 0.0;          // 0 = unchanged
    TimeInForce tif = TimeInForce::GTC;
};

// ---------------------------------------------------------------------------
// Market data subscription
// ---------------------------------------------------------------------------

struct SubscriptionRequest {
    std::string symbol;              // canonical
    bool quotes = true;
    bool trades = true;
    bool bars = true;
    bool orderBook = false;
    uint64_t throttleMicros = 0;     // 0 = no throttle
};

// ---------------------------------------------------------------------------
// Capability discovery (spec §38)
// ---------------------------------------------------------------------------

struct ConnectorCapabilities {
    bool spot = true;
    bool margin = false;
    bool futures = false;
    bool options = false;
    bool shorting = false;
    double maxLeverage = 1.0;

    // Pre-trade symbol validation data (populated per symbol by the broker).
    struct SymbolSpec {
        std::string symbol;        // canonical
        std::string nativeSymbol;  // broker symbol string
        bool tradable = false;
        bool shortable = false;
        double minQty = 0.0;
        double stepSize = 0.0;
        double tickSize = 0.0;
        double minNotional = 0.0;
        double maxLeverage = 1.0;
        bool marketHoursOnly = false;
    };
    std::vector<SymbolSpec> symbols;

    // Rate-limit posture reported by the broker (spec §37).
    struct RateLimit {
        double requestsPerSecond = 0.0;
        double requestsPerMinute = 0.0;
        double weightPerRequest = 1.0;
        uint32_t burst = 0;
    };
    RateLimit rateLimit;
};

// ---------------------------------------------------------------------------
// Connector health / test result (spec §30)
// ---------------------------------------------------------------------------

struct ConnectorHealth {
    bool connected = false;
    bool authenticated = false;
    uint64_t lastHeartbeatMs = 0;
    uint64_t lastAckLatencyUs = 0;
    std::string status;             // human-readable
    std::vector<std::string> issues;
};

} // namespace trading
