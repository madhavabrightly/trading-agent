#pragma once

// trading/execution/OrderRouter.hpp — signal -> validated order -> connector
// (spec §20, §39, §56). The ONLY component allowed to talk to a broker
// connector. Validates locally (symbol/tick/min notional/side) before sending.

#include "trading/connectors/ITradingConnector.hpp"
#include "trading/signal/TradingSignal.hpp"
#include "trading/core/Types.hpp"
#include "trading/market/MarketTypes.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trading {

// Result of local pre-trade validation (spec §39).
struct OrderValidation {
    bool ok = false;
    std::vector<std::string> errors;   // human readable, never secrets
};

class OrderValidator {
public:
    // Pure local checks against the capability spec + market snapshot.
    static OrderValidation validate(const OrderRequest& req,
                                    const ConnectorCapabilities& caps,
                                    const MarketSnapshot* market);
};

class OrderRouter {
public:
    // Builds an OrderRequest from a risk-approved signal (direction + qty).
    static OrderRequest buildRequest(const TradingSignal& signal,
                                     double approvedQuantity, Side side);

    // Route: validate -> place on the connector. Sets the connector's tick
    // callback expectations. Returns the acknowledged order (spec §21 —
    // requires broker ack, never assumes success from a 200 alone).
    static OrderResult route(ITradingConnector& connector,
                             const OrderRequest& request);

private:
    static bool symbolSupported(const ConnectorCapabilities& caps,
                                const std::string& symbol);
};

} // namespace trading
