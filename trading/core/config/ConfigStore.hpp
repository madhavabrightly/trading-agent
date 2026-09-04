#pragma once

// trading/core/config/ConfigStore.hpp — versioned JSON configuration.
// Layout mirrors the spec (§26) but JSON (no YAML dependency). Secrets are
// NEVER stored here — they live in the credential store.

#include "trading/core/Types.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace trading {

struct RiskConfig {
    double maxDailyLoss = 0.02;      // fraction of equity
    double maxPositionSize = 0.10;   // fraction of equity per symbol
    double maxOrderSize = 0.05;      // fraction of equity per order
    double maxLeverage = 5.0;
    int maxOpenPositions = 10;
    double maxDrawdown = 0.20;
    double maxExposure = 0.50;       // fraction of equity, all positions
    double maxCorrelationExposure = 0.30;
    double maxSpreadPct = 0.005;     // 0.5% spread rejection threshold
    double maxSlippagePct = 0.01;
    bool enforceMarketHours = false;
    bool killSwitchEnabled = true;
    double cooldownSeconds = 30.0;   // per-symbol cooldown after an order
};

struct OcrTargetConfig {
    std::string url;        // page to lock and watch
    std::string role;       // "price" | "news" | "chart" | "alert"
    std::string name;       // optional label, shown in UI
    double rateHz = 0.5;    // capture rate for this target
    float minConfidence = 0.6f;
};

struct AiConfig {
    bool enabled = true;
    bool ragEnabled = false;
    std::string provider = "deepseek";   // logical provider name
    std::string baseUrl;                 // e.g. https://api.deepseek.com/v1
    std::string model;                   // e.g. deepseek-chat
    double temperature = 0.2;
    int maxTokens = 2048;
    double timeoutSeconds = 30.0;
    std::string pythonServiceUrl = "http://127.0.0.1:8765";
    std::string apiKeyEnvVar = "AI_API_KEY";  // key via env / credential store
};

struct MarketConfig {
    bool enabled = true;
    double staleAfterMs = 2000.0;      // market data older than this => stale
    std::vector<std::string> symbols;  // canonical symbols to subscribe
};

struct ExecutionConfig {
    bool enabled = false;              // master switch (paper or live)
    bool cancelOpenOnKill = false;     // kill switch: cancel open orders?
    double maxRetries = 3;
};

struct LoggingConfig {
    std::string level = "info";
    bool console = true;
    std::string file = "logs/trading-core.log";
};

struct ApiConfig {
    bool enabled = true;
    std::string bindHost = "127.0.0.1";  // loopback only by default
    uint16_t port = 8890;
    std::string bearerToken;             // random per-run if empty; never logged
    bool requireAuth = true;
};

struct SystemConfig {
    EnvMode mode = EnvMode::SIMULATION;
    std::string timezone = "UTC";
    int configVersion = 1;
};

struct AppConfig {
    int version = 1;
    SystemConfig system;
    MarketConfig marketData;
    RiskConfig risk;
    ExecutionConfig execution;
    AiConfig ai;
    LoggingConfig logging;
    ApiConfig api;
    bool ocrEnabled = true;
    std::vector<OcrTargetConfig> ocrTargets;
};

class ConfigStore {
public:
    // Load from path. Returns empty optional with reason on failure.
    static std::optional<AppConfig> load(const std::string& path,
                                         std::string& error);

    // Validate semantic rules that JSON typing cannot express.
    static std::optional<std::string> validate(const AppConfig& cfg);

    // Serialize (without secrets — the config schema has none).
    static bool save(const AppConfig& cfg, const std::string& path,
                     std::string& error);

    static AppConfig defaults();

    // Applies one override in dotted form, e.g. "risk.max_leverage" = 3.
    static bool applyOverride(AppConfig& cfg, const std::string& dottedPath,
                              const nlohmann::json& value, std::string& error);

private:
    static std::optional<AppConfig> parse(const nlohmann::json& j, std::string& error);
    static nlohmann::json toJson(const AppConfig& cfg);
};

} // namespace trading
