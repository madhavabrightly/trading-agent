#include "trading/execution/OrderRouter.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>

namespace trading {

OrderValidation OrderValidator::validate(const OrderRequest& req,
                                         const ConnectorCapabilities& caps,
                                         const MarketSnapshot* market) {
    OrderValidation v;
    if (req.symbol.empty()) v.errors.push_back("empty symbol");
    if (req.quantity <= 0.0) v.errors.push_back("quantity must be positive");
    if (req.type == OrderType::LIMIT && req.limitPrice <= 0.0)
        v.errors.push_back("limit order requires a limit price");
    if (req.type == OrderType::STOP && req.stopPrice <= 0.0)
        v.errors.push_back("stop order requires a stop price");
    if (req.type == OrderType::STOP_LIMIT &&
        (req.limitPrice <= 0.0 || req.stopPrice <= 0.0))
        v.errors.push_back("stop-limit requires stop + limit prices");

    // Broker symbol support (spec §39: symbol exists, tradable).
    bool supported = false;
    bool shortable = false;
    bool found = false;
    for (const auto& s : caps.symbols) {
        if (s.symbol == req.symbol) {
            found = true;
            supported = s.tradable;
            shortable = s.shortable;
            break;
        }
    }
    if (found && !supported)
        v.errors.push_back("symbol not tradable on this connector");
    if (req.side == Side::SELL && found && !shortable && !req.reduceOnly)
        v.errors.push_back("shorting not supported on this connector");

    // Market snapshot sanity.
    if (market) {
        if (market->stale) v.errors.push_back("market data stale");
        if (req.type != OrderType::MARKET && market->last > 0.0) {
            // Price sanity band: reject limit prices >20% off market.
            double ref = market->last;
            double lim = req.limitPrice > 0.0 ? req.limitPrice : req.stopPrice;
            if (lim > 0.0 && std::fabs(lim - ref) / ref > 0.20)
                v.errors.push_back("order price >20% away from market");
        }
    }
    v.ok = v.errors.empty();
    return v;
}

bool OrderRouter::symbolSupported(const ConnectorCapabilities& caps,
                                  const std::string& symbol) {
    for (const auto& s : caps.symbols)
        if (s.symbol == symbol && s.tradable) return true;
    // If the connector has not published a symbol list (capability discovery
    // incomplete), do not block — the broker will validate.
    return caps.symbols.empty();
}

OrderRequest OrderRouter::buildRequest(const TradingSignal& signal,
                                       double approvedQuantity, Side side) {
    OrderRequest req;
    req.symbol = signal.symbol;
    req.side = side;
    req.type = OrderType::MARKET;  // starter router uses market for signals
    req.quantity = approvedQuantity;
    req.clientOrderId = "tc-" + std::to_string(signal.timestamp) + "-" +
                        signal.symbol;
    return req;
}

OrderResult OrderRouter::route(ITradingConnector& connector,
                               const OrderRequest& request) {
    OrderResult res;
    // Local validation first (never send an obviously bad order).
    auto v = OrderValidator::validate(request, connector.capabilities(),
                                      nullptr);
    if (!v.ok) {
        res.ok = false;
        for (const auto& e : v.errors) {
            res.error += (res.error.empty() ? "" : "; ") + e;
        }
        LOG_ERROR("OrderRouter: local validation failed: {}", res.error);
        return res;
    }
    // Place on the broker and require an acknowledgement.
    res = connector.placeOrder(request);
    if (res.ok) {
        LOG_INFO("OrderRouter: order {} {} {} qty={} ack={}", request.symbol,
                 toString(request.side), toString(request.type),
                 request.quantity, res.order.orderId);
    } else {
        LOG_ERROR("OrderRouter: order rejected by broker: {}", res.error);
    }
    return res;
}

} // namespace trading
