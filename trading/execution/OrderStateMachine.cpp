#include "trading/execution/OrderStateMachine.hpp"

namespace trading {

bool OrderStateMachine::isAllowed(OrderStatus from, OrderStatus to) {
    using S = OrderStatus;
    switch (from) {
        case S::CREATED:
            return to == S::VALIDATING || to == S::SUBMITTED ||
                   to == S::REJECTED || to == S::FAILED || to == S::UNKNOWN;
        case S::VALIDATING:
            return to == S::SUBMITTED || to == S::REJECTED || to == S::FAILED ||
                   to == S::UNKNOWN || to == S::CREATED;
        case S::SUBMITTED:
            return to == S::ACKNOWLEDGED || to == S::REJECTED || to == S::FAILED ||
                   to == S::UNKNOWN;
        case S::ACKNOWLEDGED:
            return to == S::PARTIALLY_FILLED || to == S::FILLED ||
                   to == S::CANCEL_REQUESTED || to == S::REJECTED ||
                   to == S::FAILED || to == S::UNKNOWN;
        case S::PARTIALLY_FILLED:
            return to == S::ACKNOWLEDGED || to == S::FILLED ||
                   to == S::CANCEL_REQUESTED || to == S::FAILED ||
                   to == S::UNKNOWN;
        case S::CANCEL_REQUESTED:
            return to == S::CANCELLED || to == S::PARTIALLY_FILLED ||
                   to == S::FILLED || to == S::ACKNOWLEDGED || to == S::UNKNOWN;
        case S::FILLED:
        case S::CANCELLED:
        case S::REJECTED:
        case S::FAILED:
            // Terminal states only move to UNKNOWN (via reconciliation).
            return to == S::UNKNOWN;
        case S::UNKNOWN:
            return to == S::ACKNOWLEDGED || to == S::PARTIALLY_FILLED ||
                   to == S::FILLED || to == S::CANCELLED || to == S::REJECTED ||
                   to == S::FAILED || to == S::SUBMITTED;
    }
    return false;
}

bool OrderStateMachine::requiresAck(OrderStatus from, OrderStatus to) {
    using S = OrderStatus;
    // Broker-terminal transitions (FILLED/CANCELLED) must be preceded by an
    // ACKNOWLEDGED (or PARTIALLY_FILLED, which implies one). Local pre-ack
    // failure (REJECTED/FAILED) is allowed directly from any pre-ack state.
    bool terminal = to == S::FILLED || to == S::CANCELLED;
    if (!terminal) return false;
    return from != S::ACKNOWLEDGED && from != S::PARTIALLY_FILLED &&
           from != S::CANCEL_REQUESTED;
}

} // namespace trading
