// Unit tests for the trading config store (Phase 2).
#include "trading/core/config/ConfigStore.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace trading;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

int main() {
    // Defaults
    AppConfig d = ConfigStore::defaults();
    CHECK(d.system.mode == EnvMode::SIMULATION, "default mode is SIMULATION");
    CHECK(d.execution.enabled == false, "execution disabled by default");
    CHECK(d.risk.maxDailyLoss == 0.02, "default daily loss cap");
    CHECK(d.api.bindHost == "127.0.0.1", "api binds loopback by default");
    CHECK(d.ocrTargets.empty(), "no OCR targets by default");

    // Validation catches nonsense
    AppConfig bad = d;
    bad.risk.maxDailyLoss = 1.5;
    auto verr = ConfigStore::validate(bad);
    CHECK(verr.has_value(), "validate rejects max_daily_loss > 1");
    bad = d;
    bad.api.bindHost = "0.0.0.0";
    verr = ConfigStore::validate(bad);
    CHECK(verr.has_value(), "validate rejects non-loopback bind");

    // Round-trip through a temp file
    AppConfig c = d;
    c.system.mode = EnvMode::PAPER;
    c.system.timezone = "Asia/Kolkata";
    c.marketData.symbols = {"BTC/USD", "AAPL/USD"};
    c.risk.maxLeverage = 3.0;
    c.ocrEnabled = true;
    c.ocrTargets.push_back({"https://www.binance.com/en/trade/BTC_USDT", "price", "Binance", 0.5, 0.6f});
    c.ai.baseUrl = "https://api.deepseek.com/v1";
    c.ai.model = "deepseek-chat";
    c.execution.enabled = false;
    c.logging.level = "info";

    std::string path = (std::filesystem::temp_directory_path() / "trading_cfg_test.json").string();
    std::string err;
    CHECK(ConfigStore::save(c, path, err), "config save succeeds");
    auto loaded = ConfigStore::load(path, err);
    CHECK(loaded.has_value(), "config load succeeds");
    if (loaded) {
        CHECK(loaded->system.mode == EnvMode::PAPER, "mode round-trips");
        CHECK(loaded->system.timezone == "Asia/Kolkata", "timezone round-trips");
        CHECK(loaded->risk.maxLeverage == 3.0, "leverage round-trips");
        CHECK(loaded->marketData.symbols.size() == 2, "symbols round-trip");
        CHECK(loaded->ocrTargets.size() == 1, "ocr targets round-trip");
        CHECK(loaded->ocrTargets[0].role == "price", "ocr role round-trips");
        CHECK(loaded->ai.model == "deepseek-chat", "ai model round-trips");
        // Secrets must never appear
        std::ifstream raw(path);
        std::string content((std::istreambuf_iterator<char>(raw)),
                            std::istreambuf_iterator<char>());
        CHECK(content.find("api_key") == std::string::npos, "no api_key key serialized");
        CHECK(content.find("secret") == std::string::npos, "no secret serialized");
    }

    // Parse error handling
    std::string badPath = (std::filesystem::temp_directory_path() / "trading_cfg_bad.json").string();
    std::ofstream b(badPath);
    b << "{ not json !!!";
    b.close();
    auto nb = ConfigStore::load(badPath, err);
    CHECK(!nb.has_value() && !err.empty(), "malformed json produces error");

    std::string badModePath = (std::filesystem::temp_directory_path() / "trading_cfg_badmode.json").string();
    std::ofstream bm(badModePath);
    bm << R"({"version":1,"system":{"mode":"PANIC"}})";
    bm.close();
    auto nm = ConfigStore::load(badModePath, err);
    CHECK(!nm.has_value(), "invalid mode string rejected");

    // Overrides
    AppConfig o = d;
    CHECK(ConfigStore::applyOverride(o, "risk.max_leverage", 2.0, err), "override leverage");
    CHECK(o.risk.maxLeverage == 2.0, "override value applied");
    CHECK(!ConfigStore::applyOverride(o, "nonexistent.path", 1, err), "unknown override rejected");

    std::remove(path.c_str());
    std::remove(badPath.c_str());
    std::remove(badModePath.c_str());

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
