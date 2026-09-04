#pragma once

// trading/execution/OrderStateMachine.hpp — authoritative order lifecycle
// (spec §21). Orders never move to FILLED/CANCELLED on a local assumption;
// transitions require an explicit broker acknowledgement. Invalid transitions
// are rejected.

#include "trading/core/Types.hpp"
#include <optional>
#include <string>
#include <vector>

namespace trading {

class OrderStateMachine {
public:
    OrderStateMachine() = default;
    explicit OrderStateMachine(OrderStatus initial) : state_(initial) {}

    OrderStatus state() const { return state_; }

    // Request a state transition. Allowed transitions encode the lifecycle:
    //   CREATED -> VALIDATING -> SUBMITTED -> ACKNOWLEDGED
    //   ACKNOWLEDGED <-> PARTIALLY_FILLED -> FILLED
    //   ACKNOWLEDGED/PARTIALLY_FILLED -> CANCEL_REQUESTED -> CANCELLED
    //   any pre-ack -> FAILED / REJECTED
    //   any -> UNKNOWN (reconciliation required)
    bool transition(OrderStatus next, std::string& error) {
        if (state_ == next) {
            error = std::string("order already in state ") + toString(next);
            return false;
        }
        // Acknowledge-before-terminal rule: never jump straight to a broker-
        // terminal state (FILLED/CANCELLED) from a pre-ack state.
        if (requiresAck(state_, next)) {
            error = std::string("order requires ACKNOWLEDGED before ") +
                    toString(next);
            return false;
        }
        if (!isAllowed(state_, next)) {
            error = "invalid order transition " +
                    std::string(toString(state_)) + " -> " + toString(next);
            return false;
        }
        state_ = next;
        history_.push_back(next);
        return true;
    }

    // Convenience: true only for terminal success states.
    bool isTerminal() const {
        return state_ == OrderStatus::FILLED ||
               state_ == OrderStatus::CANCELLED ||
               state_ == OrderStatus::REJECTED ||
               state_ == OrderStatus::FAILED;
    }

    bool isOpen() const {
        return state_ == OrderStatus::CREATED ||
               state_ == OrderStatus::VALIDATING ||
               state_ == OrderStatus::SUBMITTED ||
               state_ == OrderStatus::ACKNOWLEDGED ||
               state_ == OrderStatus::PARTIALLY_FILLED ||
               state_ == OrderStatus::CANCEL_REQUESTED;
    }

    // When an order sits in an unexpected state, mark UNKNOWN so
    // reconciliation runs (spec §22). Never silently "fix" it.
    void markUnknown() { state_ = OrderStatus::UNKNOWN; }

    const std::vector<OrderStatus>& history() const { return history_; }

private:
    static bool isAllowed(OrderStatus from, OrderStatus to);
    static bool requiresAck(OrderStatus from, OrderStatus to);

    OrderStatus state_ = OrderStatus::CREATED;
    std::vector<OrderStatus> history_;
};

} // namespace trading
