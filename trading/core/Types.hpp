#pragma once

// trading/Types.hpp — shared vocabulary for the trading core. Independent of
// the existing edgemon:: types; both namespaces coexist in one process.

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>

namespace trading {

// ---------------------------------------------------------------------------
// Environment / mode
// ---------------------------------------------------------------------------

// The environment a connector is running against. Never silently changed.
enum class EnvMode {
    SIMULATION,   // internal simulator, no network
    PAPER,        // broker-provided paper account
    TESTNET,      // broker testnet environment
    LIVE          // real money — requires explicit authorization
};

constexpr const char* toString(EnvMode m) {
    switch (m) {
        case EnvMode::SIMULATION: return "SIMULATION";
        case EnvMode::PAPER:      return "PAPER";
        case EnvMode::TESTNET:    return "TESTNET";
        case EnvMode::LIVE:       return "LIVE";
    }
    return "UNKNOWN";
}

inline std::optional<EnvMode> envModeFromString(const std::string& s) {
    if (s == "SIMULATION" || s == "simulation" || s == "sim") return EnvMode::SIMULATION;
    if (s == "PAPER" || s == "paper") return EnvMode::PAPER;
    if (s == "TESTNET" || s == "testnet") return EnvMode::TESTNET;
    if (s == "LIVE" || s == "live") return EnvMode::LIVE;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Orders
// ---------------------------------------------------------------------------

enum class Side { BUY, SELL };

constexpr const char* toString(Side s) {
    return s == Side::BUY ? "BUY" : "SELL";
}

inline std::optional<Side> sideFromString(const std::string& s) {
    if (s == "BUY" || s == "buy" || s == "B" || s == "b") return Side::BUY;
    if (s == "SELL" || s == "sell" || s == "S" || s == "s") return Side::SELL;
    return std::nullopt;
}

enum class OrderType {
    MARKET,
    LIMIT,
    STOP,
    STOP_LIMIT,
    TAKE_PROFIT,
    STOP_LOSS
};

constexpr const char* toString(OrderType t) {
    switch (t) {
        case OrderType::MARKET:     return "MARKET";
        case OrderType::LIMIT:      return "LIMIT";
        case OrderType::STOP:       return "STOP";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
        case OrderType::TAKE_PROFIT:return "TAKE_PROFIT";
        case OrderType::STOP_LOSS:  return "STOP_LOSS";
    }
    return "UNKNOWN";
}

inline std::optional<OrderType> orderTypeFromString(const std::string& s) {
    if (s == "MARKET") return OrderType::MARKET;
    if (s == "LIMIT")  return OrderType::LIMIT;
    if (s == "STOP")   return OrderType::STOP;
    if (s == "STOP_LIMIT") return OrderType::STOP_LIMIT;
    if (s == "TAKE_PROFIT") return OrderType::TAKE_PROFIT;
    if (s == "STOP_LOSS")   return OrderType::STOP_LOSS;
    return std::nullopt;
}

enum class TimeInForce {
    GTC,   // good till cancelled
    IOC,   // immediate or cancel
    FOK,   // fill or kill
    DAY,   // day
    OPG,   // at the open
    CLS    // at the close
};

constexpr const char* toString(TimeInForce t) {
    switch (t) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::DAY: return "DAY";
        case TimeInForce::OPG: return "OPG";
        case TimeInForce::CLS: return "CLS";
    }
    return "GTC";
}

// Order lifecycle — spec §21. An order never moves to a terminal "success"
// state without a broker/exchange acknowledgement.
enum class OrderStatus {
    CREATED,            // local, not yet validated
    VALIDATING,         // passing through OrderValidator / RiskEngine
    SUBMITTED,          // request sent to broker
    ACKNOWLEDGED,       // broker accepted
    PARTIALLY_FILLED,   // some quantity filled
    FILLED,             // fully filled
    CANCEL_REQUESTED,   // cancel sent, awaiting ack
    CANCELLED,          // broker confirmed cancelled
    REJECTED,           // broker rejected
    FAILED,             // local/transport failure
    UNKNOWN             // state cannot be determined — reconcile
};

constexpr const char* toString(OrderStatus s) {
    switch (s) {
        case OrderStatus::CREATED:          return "CREATED";
        case OrderStatus::VALIDATING:       return "VALIDATING";
        case OrderStatus::SUBMITTED:        return "SUBMITTED";
        case OrderStatus::ACKNOWLEDGED:     return "ACKNOWLEDGED";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCEL_REQUESTED: return "CANCEL_REQUESTED";
        case OrderStatus::CANCELLED:        return "CANCELLED";
        case OrderStatus::REJECTED:         return "REJECTED";
        case OrderStatus::FAILED:           return "FAILED";
        case OrderStatus::UNKNOWN:          return "UNKNOWN";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Events — unified bus event types (spec §11)
// ---------------------------------------------------------------------------

enum class EventType {
    MARKET_TICK,
    ORDERBOOK,
    TRADE,
    NEWS,
    OCR,
    SIGNAL,
    ORDER,
    EXECUTION,
    POSITION,
    RISK,
    SYSTEM,
    DEBUG,
    TELEMETRY
};

constexpr const char* toString(EventType t) {
    switch (t) {
        case EventType::MARKET_TICK: return "MARKET_TICK";
        case EventType::ORDERBOOK:    return "ORDERBOOK";
        case EventType::TRADE:        return "TRADE";
        case EventType::NEWS:         return "NEWS";
        case EventType::OCR:          return "OCR";
        case EventType::SIGNAL:       return "SIGNAL";
        case EventType::ORDER:        return "ORDER";
        case EventType::EXECUTION:    return "EXECUTION";
        case EventType::POSITION:     return "POSITION";
        case EventType::RISK:         return "RISK";
        case EventType::SYSTEM:       return "SYSTEM";
        case EventType::DEBUG:        return "DEBUG";
        case EventType::TELEMETRY:    return "TELEMETRY";
    }
    return "UNKNOWN";
}

// Relative importance for the priority event bus (spec §12). Higher priority
// classes are drained first and are far less likely to drop under load.
enum class EventPriority {
    HIGH,   // ORDER, EXECUTION, RISK, POSITION
    MEDIUM, // MARKET_TICK, SIGNAL, NEWS, TRADE, ORDERBOOK
    LOW     // DEBUG, TELEMETRY, OCR-informational
};

inline EventPriority priorityOf(EventType t) {
    switch (t) {
        case EventType::ORDER:
        case EventType::EXECUTION:
        case EventType::RISK:
        case EventType::POSITION:
            return EventPriority::HIGH;
        case EventType::MARKET_TICK:
        case EventType::ORDERBOOK:
        case EventType::TRADE:
        case EventType::NEWS:
        case EventType::SIGNAL:
            return EventPriority::MEDIUM;
        case EventType::OCR:
        case EventType::SYSTEM:
        case EventType::DEBUG:
        case EventType::TELEMETRY:
        default:
            return EventPriority::LOW;
    }
}

// ---------------------------------------------------------------------------
// Risk decisions
// ---------------------------------------------------------------------------

struct RiskDecision {
    bool approved = false;
    std::string reason;             // human readable, logged
    double allowedQuantity = 0.0;   // risk-approved quantity (<= requested)
    double reasonCode = 0;          // stable machine code (0 = approved)
};

} // namespace trading
