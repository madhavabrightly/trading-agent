#include "trading/core/config/ConfigStore.hpp"
#include "logger.hpp"
#include <fstream>
#include <sstream>
#include <set>

namespace trading {

namespace {

using nlohmann::json;

// Reads a string field with fallback; missing => default, wrong type => error.
std::optional<std::string> getString(const json& j, const char* key,
                                     std::string& error) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    if (!it->is_string()) {
        error = std::string("field '") + key + "' must be a string";
        return std::nullopt; // distinguished from "missing" by error set
    }
    return it->get<std::string>();
}

bool hasError(const std::string& e) { return !e.empty(); }

double getNum(const json& j, const char* key, double dflt, std::string& error) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return dflt;
    if (!it->is_number()) {
        if (error.empty()) error = std::string("field '") + key + "' must be a number";
        return dflt;
    }
    return it->get<double>();
}

int getInt(const json& j, const char* key, int dflt, std::string& error) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return dflt;
    if (!it->is_number()) {
        if (error.empty()) error = std::string("field '") + key + "' must be an integer";
        return dflt;
    }
    if (!it->is_number_integer()) {
        // Accept 3.0 as 3 — the JSON writer emits doubles for mixed int/float.
        double v = it->get<double>();
        return static_cast<int>(v);
    }
    return it->get<int>();
}

bool getBool(const json& j, const char* key, bool dflt, std::string& error) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return dflt;
    if (!it->is_boolean()) {
        if (error.empty()) error = std::string("field '") + key + "' must be a boolean";
        return dflt;
    }
    return it->get<bool>();
}

const json* section(const json& j, const char* name) {
    auto it = j.find(name);
    return (it != j.end() && it->is_object()) ? &(*it) : nullptr;
}

} // namespace

AppConfig ConfigStore::defaults() {
    return AppConfig{};
}

nlohmann::json ConfigStore::toJson(const AppConfig& cfg) {
    json j;
    j["version"] = cfg.version;
    j["system"] = {
        {"mode", toString(cfg.system.mode)},
        {"timezone", cfg.system.timezone}
    };
    j["market_data"] = {
        {"enabled", cfg.marketData.enabled},
        {"stale_after_ms", cfg.marketData.staleAfterMs},
        {"symbols", cfg.marketData.symbols}
    };
    json ocrTargets = json::array();
    for (const auto& t : cfg.ocrTargets) {
        ocrTargets.push_back({
            {"url", t.url},
            {"role", t.role},
            {"name", t.name},
            {"rate_hz", t.rateHz},
            {"min_confidence", t.minConfidence}
        });
    }
    j["ocr"] = {
        {"enabled", cfg.ocrEnabled},
        {"targets", ocrTargets}
    };
    j["ai"] = {
        {"enabled", cfg.ai.enabled},
        {"rag_enabled", cfg.ai.ragEnabled},
        {"provider", cfg.ai.provider},
        {"base_url", cfg.ai.baseUrl},
        {"model", cfg.ai.model},
        {"temperature", cfg.ai.temperature},
        {"max_tokens", cfg.ai.maxTokens},
        {"timeout_seconds", cfg.ai.timeoutSeconds},
        {"python_service_url", cfg.ai.pythonServiceUrl}
        // NOTE: api key is intentionally NOT serialized
    };
    j["risk"] = {
        {"max_daily_loss", cfg.risk.maxDailyLoss},
        {"max_position_size", cfg.risk.maxPositionSize},
        {"max_order_size", cfg.risk.maxOrderSize},
        {"max_leverage", cfg.risk.maxLeverage},
        {"max_open_positions", cfg.risk.maxOpenPositions},
        {"max_drawdown", cfg.risk.maxDrawdown},
        {"max_exposure", cfg.risk.maxExposure},
        {"max_correlation_exposure", cfg.risk.maxCorrelationExposure},
        {"max_spread_pct", cfg.risk.maxSpreadPct},
        {"max_slippage_pct", cfg.risk.maxSlippagePct},
        {"enforce_market_hours", cfg.risk.enforceMarketHours},
        {"kill_switch_enabled", cfg.risk.killSwitchEnabled},
        {"cooldown_seconds", cfg.risk.cooldownSeconds}
    };
    j["execution"] = {
        {"enabled", cfg.execution.enabled},
        {"cancel_open_on_kill", cfg.execution.cancelOpenOnKill},
        {"max_retries", cfg.execution.maxRetries}
    };
    j["logging"] = {
        {"level", cfg.logging.level},
        {"console", cfg.logging.console},
        {"file", cfg.logging.file}
    };
    j["api"] = {
        {"enabled", cfg.api.enabled},
        {"bind_host", cfg.api.bindHost},
        {"port", cfg.api.port},
        {"require_auth", cfg.api.requireAuth}
        // bearer token is not persisted to config
    };
    return j;
}

std::optional<AppConfig> ConfigStore::parse(const json& j, std::string& error) {
    AppConfig cfg = defaults();
    if (j.is_null() || !j.is_object()) {
        error = "config root must be a JSON object";
        return std::nullopt;
    }
    cfg.version = getInt(j, "version", 1, error);
    if (cfg.version != 1) {
        error = "unsupported config version: " + std::to_string(cfg.version);
        return std::nullopt;
    }

    if (const json* s = section(j, "system")) {
        if (auto v = getString(*s, "mode", error); v && hasError(error) == false) {
            if (auto m = envModeFromString(*v)) cfg.system.mode = *m;
            else if (hasError(error) == false) error = "invalid system.mode: " + *v;
        }
        if (auto tz = getString(*s, "timezone", error); tz) cfg.system.timezone = *tz;
    }
    if (const json* md = section(j, "market_data")) {
        cfg.marketData.enabled = getBool(*md, "enabled", true, error);
        cfg.marketData.staleAfterMs = getNum(*md, "stale_after_ms", 2000.0, error);
        if (auto it = md->find("symbols"); it != md->end() && it->is_array()) {
            cfg.marketData.symbols.clear();
            for (const auto& e : *it)
                if (e.is_string()) cfg.marketData.symbols.push_back(e.get<std::string>());
        }
    }
    if (const json* ocr = section(j, "ocr")) {
        cfg.ocrEnabled = getBool(*ocr, "enabled", true, error);
        if (auto it = ocr->find("targets"); it != ocr->end() && it->is_array()) {
            cfg.ocrTargets.clear();
            for (const auto& e : *it) {
                if (!e.is_object()) continue;
                OcrTargetConfig t;
                t.url = getString(e, "url", error).value_or("");
                t.role = getString(e, "role", error).value_or("news");
                t.name = getString(e, "name", error).value_or("");
                t.rateHz = getNum(e, "rate_hz", 0.5, error);
                t.minConfidence = static_cast<float>(getNum(e, "min_confidence", 0.6, error));
                if (!t.url.empty()) cfg.ocrTargets.push_back(std::move(t));
            }
        }
    }
    if (const json* a = section(j, "ai")) {
        cfg.ai.enabled = getBool(*a, "enabled", true, error);
        cfg.ai.ragEnabled = getBool(*a, "rag_enabled", false, error);
        cfg.ai.provider = getString(*a, "provider", error).value_or("deepseek");
        cfg.ai.baseUrl = getString(*a, "base_url", error).value_or("");
        cfg.ai.model = getString(*a, "model", error).value_or("");
        cfg.ai.temperature = getNum(*a, "temperature", 0.2, error);
        cfg.ai.maxTokens = getInt(*a, "max_tokens", 2048, error);
        cfg.ai.timeoutSeconds = getNum(*a, "timeout_seconds", 30.0, error);
        cfg.ai.pythonServiceUrl = getString(*a, "python_service_url", error)
                                      .value_or("http://127.0.0.1:8765");
        cfg.ai.apiKeyEnvVar = getString(*a, "api_key_env", error).value_or("AI_API_KEY");
    }
    if (const json* r = section(j, "risk")) {
        cfg.risk.maxDailyLoss = getNum(*r, "max_daily_loss", 0.02, error);
        cfg.risk.maxPositionSize = getNum(*r, "max_position_size", 0.10, error);
        cfg.risk.maxOrderSize = getNum(*r, "max_order_size", 0.05, error);
        cfg.risk.maxLeverage = getNum(*r, "max_leverage", 5.0, error);
        cfg.risk.maxOpenPositions = getInt(*r, "max_open_positions", 10, error);
        cfg.risk.maxDrawdown = getNum(*r, "max_drawdown", 0.20, error);
        cfg.risk.maxExposure = getNum(*r, "max_exposure", 0.50, error);
        cfg.risk.maxCorrelationExposure =
            getNum(*r, "max_correlation_exposure", 0.30, error);
        cfg.risk.maxSpreadPct = getNum(*r, "max_spread_pct", 0.005, error);
        cfg.risk.maxSlippagePct = getNum(*r, "max_slippage_pct", 0.01, error);
        cfg.risk.enforceMarketHours = getBool(*r, "enforce_market_hours", false, error);
        cfg.risk.killSwitchEnabled = getBool(*r, "kill_switch_enabled", true, error);
        cfg.risk.cooldownSeconds = getNum(*r, "cooldown_seconds", 30.0, error);
    }
    if (const json* e = section(j, "execution")) {
        cfg.execution.enabled = getBool(*e, "enabled", false, error);
        cfg.execution.cancelOpenOnKill = getBool(*e, "cancel_open_on_kill", false, error);
        cfg.execution.maxRetries = getInt(*e, "max_retries", 3, error);
    }
    if (const json* l = section(j, "logging")) {
        cfg.logging.level = getString(*l, "level", error).value_or("info");
        cfg.logging.console = getBool(*l, "console", true, error);
        cfg.logging.file = getString(*l, "file", error).value_or("logs/trading-core.log");
    }
    if (const json* a = section(j, "api")) {
        cfg.api.enabled = getBool(*a, "enabled", true, error);
        cfg.api.bindHost = getString(*a, "bind_host", error).value_or("127.0.0.1");
        cfg.api.port = static_cast<uint16_t>(getInt(*a, "port", 8890, error));
        cfg.api.requireAuth = getBool(*a, "require_auth", true, error);
    }

    if (hasError(error)) return std::nullopt;
    return cfg;
}

std::optional<std::string> ConfigStore::validate(const AppConfig& cfg) {
    if (cfg.risk.maxDailyLoss < 0 || cfg.risk.maxDailyLoss > 1.0)
        return "risk.max_daily_loss must be in [0,1]";
    if (cfg.risk.maxPositionSize < 0 || cfg.risk.maxPositionSize > 1.0)
        return "risk.max_position_size must be in [0,1]";
    if (cfg.risk.maxOrderSize < 0 || cfg.risk.maxOrderSize > 1.0)
        return "risk.max_order_size must be in [0,1]";
    if (cfg.risk.maxLeverage < 1.0)
        return "risk.max_leverage must be >= 1";
    if (cfg.risk.maxSpreadPct < 0 || cfg.risk.maxSlippagePct < 0)
        return "spread/slippage thresholds must be >= 0";
    if (cfg.risk.cooldownSeconds < 0)
        return "risk.cooldown_seconds must be >= 0";
    if (cfg.marketData.staleAfterMs <= 0)
        return "market_data.stale_after_ms must be > 0";
    if (cfg.api.port == 0)
        return "api.port must be non-zero";
    if (!cfg.api.bindHost.empty() && cfg.api.bindHost != "127.0.0.1" &&
        cfg.api.bindHost != "localhost" && cfg.api.bindHost != "::1")
        return "api.bind_host must be loopback-only";
    for (const auto& t : cfg.ocrTargets) {
        if (t.url.empty()) return "ocr target with empty url";
        if (t.role != "price" && t.role != "news" && t.role != "chart" && t.role != "alert")
            return "ocr target role must be price|news|chart|alert: " + t.url;
        if (t.rateHz <= 0 || t.rateHz > 10)
            return "ocr target rate_hz must be in (0,10]: " + t.url;
    }
    return std::nullopt;
}

std::optional<AppConfig> ConfigStore::load(const std::string& path,
                                           std::string& error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        error = "cannot open config file: " + path;
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json j;
    try {
        j = json::parse(ss.str());
    } catch (const std::exception& e) {
        error = std::string("config parse error: ") + e.what();
        return std::nullopt;
    }
    auto cfg = parse(j, error);
    if (!cfg) return std::nullopt;
    if (auto verr = validate(*cfg)) {
        error = *verr;
        return std::nullopt;
    }
    return cfg;
}

bool ConfigStore::save(const AppConfig& cfg, const std::string& path,
                       std::string& error) {
    std::ofstream out(path);
    if (!out.is_open()) {
        error = "cannot write config file: " + path;
        return false;
    }
    out << toJson(cfg).dump(2) << "\n";
    return true;
}

bool ConfigStore::applyOverride(AppConfig& cfg, const std::string& dottedPath,
                                const nlohmann::json& value, std::string& error) {
    // Overrides target known scalar fields only (deliberately narrow).
    if (dottedPath == "system.mode") {
        if (!value.is_string()) { error = "system.mode must be a string"; return false; }
        auto m = envModeFromString(value.get<std::string>());
        if (!m) { error = "invalid mode"; return false; }
        cfg.system.mode = *m;
        return true;
    }
    if (dottedPath == "execution.enabled") {
        if (!value.is_boolean()) { error = "execution.enabled must be boolean"; return false; }
        cfg.execution.enabled = value.get<bool>();
        return true;
    }
    if (dottedPath == "risk.max_leverage") {
        if (!value.is_number()) { error = "risk.max_leverage must be a number"; return false; }
        cfg.risk.maxLeverage = value.get<double>();
        return true;
    }
    if (dottedPath == "risk.max_daily_loss") {
        if (!value.is_number()) { error = "risk.max_daily_loss must be a number"; return false; }
        cfg.risk.maxDailyLoss = value.get<double>();
        return true;
    }
    if (dottedPath == "risk.kill_switch_enabled") {
        if (!value.is_boolean()) { error = "kill_switch_enabled must be boolean"; return false; }
        cfg.risk.killSwitchEnabled = value.get<bool>();
        return true;
    }
    if (dottedPath == "market_data.enabled") {
        if (!value.is_boolean()) { error = "market_data.enabled must be boolean"; return false; }
        cfg.marketData.enabled = value.get<bool>();
        return true;
    }
    if (dottedPath == "ai.enabled") {
        if (!value.is_boolean()) { error = "ai.enabled must be boolean"; return false; }
        cfg.ai.enabled = value.get<bool>();
        return true;
    }
    if (dottedPath == "ocr.enabled") {
        if (!value.is_boolean()) { error = "ocr.enabled must be boolean"; return false; }
        cfg.ocrEnabled = value.get<bool>();
        return true;
    }
    error = "unsupported override path: " + dottedPath;
    return false;
}

} // namespace trading
