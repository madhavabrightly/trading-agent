// Unit tests for connector registry + Alpaca connector (Phases 4/5).
// Network-free tests: registry lifecycle, factory, environment tagging.
// Live-API tests are integration-gated and skipped unless ALPACA keys exist.
#include "trading/connectors/ConnectorRegistry.hpp"
#include "trading/connectors/alpaca/AlpacaConnector.hpp"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace trading;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

int main() {
    auto& reg = ConnectorRegistry::instance();

    // Register a factory for alpaca
    reg.registerFactory("alpaca", [](const ConnectorConfig& cfg) {
        return std::make_shared<AlpacaConnector>(cfg);
    });
    auto types = reg.supportedTypes();
    bool hasAlpaca = false;
    for (const auto& t : types) if (t == "alpaca") hasAlpaca = true;
    CHECK(hasAlpaca, "registry reports alpaca factory");

    // Create via factory (no network)
    ConnectorConfig cfg;
    cfg.name = "alpaca";
    cfg.env = EnvMode::PAPER;
    TradingConnectorPtr conn;
    try {
        conn = reg.create("alpaca", cfg);
    } catch (const std::exception& e) {
        std::printf("create threw: %s\n", e.what());
    }
    CHECK(conn != nullptr, "registry creates alpaca connector");
    if (conn) {
        CHECK(conn->name() == "alpaca", "connector name");
        CHECK(conn->environment() == EnvMode::PAPER, "paper env by default");
        CHECK(!conn->isConnected(), "not connected before connect()");
    }

    // Unknown type throws
    bool threw = false;
    try {
        reg.create("nonexistent_broker", cfg);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw, "unknown connector type throws");

    // connect() without credentials must fail cleanly (no crash, no network)
    if (conn) {
        bool ok = conn->connect();
        CHECK(!ok, "connect fails cleanly without credentials");
    }

    // track/find round-trip
    if (conn) {
        reg.track(conn);
        auto found = reg.find("alpaca", EnvMode::PAPER);
        CHECK(found.has_value(), "registry find returns tracked connector");
        reg.untrack("alpaca", EnvMode::PAPER);
        auto gone = reg.find("alpaca", EnvMode::PAPER);
        CHECK(!gone.has_value(), "registry untrack removes connector");
    }

    // Environment tagging never confuses paper vs live
    ConnectorConfig liveCfg;
    liveCfg.name = "alpaca";
    liveCfg.env = EnvMode::LIVE;
    auto live = reg.create("alpaca", liveCfg);
    CHECK(live->environment() == EnvMode::LIVE, "live env preserved");
    CHECK(conn && conn->environment() == EnvMode::PAPER,
          "paper instance stays paper");

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
