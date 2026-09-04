// Unit tests for execution + portfolio (Phases 12/13): state machine
// transitions, paper fills/positions/P&L, reconciliation detection.
#include "trading/execution/OrderStateMachine.hpp"
#include "trading/execution/PaperSimulator.hpp"
#include "trading/execution/OrderRouter.hpp"
#include "trading/portfolio/PortfolioEngine.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace trading;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

int main() {
    // --- Order state machine ---
    OrderStateMachine sm;
    std::string err;
    CHECK(sm.transition(OrderStatus::VALIDATING, err), "CREATED->VALIDATING ok");
    CHECK(sm.transition(OrderStatus::SUBMITTED, err), "VALIDATING->SUBMITTED ok");
    CHECK(sm.transition(OrderStatus::ACKNOWLEDGED, err), "SUBMITTED->ACKNOWLEDGED ok");
    CHECK(sm.transition(OrderStatus::FILLED, err), "ACKNOWLEDGED->FILLED ok");
    CHECK(sm.isTerminal(), "FILLED is terminal");

    OrderStateMachine sm2;
    CHECK(!sm2.transition(OrderStatus::FILLED, err), "CREATED->FILLED rejected (no ack)");
    CHECK(err.find("ACKNOWLEDGED") != std::string::npos, "error explains ack requirement");

    OrderStateMachine sm3;
    sm3.transition(OrderStatus::VALIDATING, err);
    sm3.transition(OrderStatus::SUBMITTED, err);
    CHECK(!sm3.transition(OrderStatus::CANCELLED, err), "SUBMITTED->CANCELLED rejected");
    sm3.transition(OrderStatus::ACKNOWLEDGED, err);
    sm3.transition(OrderStatus::CANCEL_REQUESTED, err);
    CHECK(sm3.transition(OrderStatus::CANCELLED, err), "CANCEL_REQUESTED->CANCELLED ok");

    // --- Paper simulator fills + position + P&L ---
    PaperSimulator paper;
    paper.connect();
    CHECK(paper.isConnected(), "paper connects");

    // Fund: place a buy market order, then feed a tick to fill it.
    OrderRequest buy;
    buy.symbol = "BTC/USD";
    buy.side = Side::BUY;
    buy.type = OrderType::MARKET;
    buy.quantity = 0.5;
    auto placed = paper.placeOrder(buy);
    CHECK(placed.ok, "paper acknowledges buy");
    CHECK(placed.order.status == OrderStatus::ACKNOWLEDGED,
          "paper order acknowledged (never assumed filled)");

    MarketTick tick;
    tick.symbol = "BTC/USD";
    tick.bid = 49'900.0;
    tick.ask = 50'100.0;
    tick.last = 50'000.0;
    paper.onMarketTick(tick);

    auto pos = paper.getPositions();
    CHECK(pos.size() == 1, "buy filled -> one position");
    if (!pos.empty()) {
        CHECK(std::fabs(pos[0].quantity - 0.5) < 1e-9, "position quantity 0.5 BTC");
        CHECK(pos[0].quantity > 0.0, "buy yields positive (long) quantity");
    }
    auto openOrders = paper.getOpenOrders();
    CHECK(openOrders.empty(), "filled order no longer open");

    // Sell to close -> P&L realized.
    OrderRequest sell;
    sell.symbol = "BTC/USD";
    sell.side = Side::SELL;
    sell.type = OrderType::MARKET;
    sell.quantity = 0.5;
    auto placedSell = paper.placeOrder(sell);
    CHECK(placedSell.ok, "paper acknowledges sell");
    MarketTick tick2;
    tick2.symbol = "BTC/USD";
    tick2.bid = 51'000.0;
    tick2.ask = 51'100.0;
    tick2.last = 51'000.0;
    paper.onMarketTick(tick2);
    auto posAfter = paper.getPositions();
    CHECK(posAfter.empty(), "sell closes position");
    auto acct = paper.getAccount();
    // Bought 0.5 @ ~50100 (ask) = 25050; sold 0.5 @ ~51000 (bid) = 25500; profit ~450
    CHECK(std::fabs(acct.cash - (100'000.0 + 450.0)) < 100.0,
          "cash reflects realized profit (within slippage)");

    // --- Order router local validation ---
    ConnectorCapabilities caps;
    ConnectorCapabilities::SymbolSpec spec;
    spec.symbol = "BTC/USD";
    spec.tradable = true;
    spec.shortable = true;
    caps.symbols = {spec};
    OrderRequest bad;
    bad.symbol = "BTC/USD";
    bad.quantity = -1.0;  // invalid
    auto v = OrderValidator::validate(bad, caps, nullptr);
    CHECK(!v.ok, "validator rejects negative qty");

    // --- Reconciliation ---
    Order local;
    local.orderId = "b1";
    local.status = OrderStatus::FILLED;
    local.filledQuantity = 1.0;
    Order brokerSame = local;

    Order brokerDiff = local;
    brokerDiff.status = OrderStatus::ACKNOWLEDGED;  // mismatch!

    auto issues = PortfolioEngine::reconcile({local}, {brokerDiff}, {}, {});
    bool foundOrderIssue = false;
    for (const auto& i : issues)
        if (i.kind == "order") foundOrderIssue = true;
    CHECK(foundOrderIssue, "reconcile flags local-vs-broker order mismatch");

    auto clean = PortfolioEngine::reconcile({local}, {brokerSame}, {}, {});
    CHECK(clean.empty(), "reconcile clean when state matches");

    // Position mismatch.
    Position lp, bp;
    lp.symbol = "BTC/USD"; lp.quantity = 1.0;
    bp.symbol = "BTC/USD"; bp.quantity = 2.0;
    auto posIssues = PortfolioEngine::reconcile({}, {}, {lp}, {bp});
    bool foundPosIssue = false;
    for (const auto& i : posIssues)
        if (i.kind == "position") foundPosIssue = true;
    CHECK(foundPosIssue, "reconcile flags position qty mismatch");

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
