#include "trading/ai/ILLMProvider.hpp"
#include "trading/net/HttpClient.hpp"
#include "trading/core/security/CredentialStore.hpp"
#include "logger.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>

namespace trading {

namespace {
std::string resolveKey(const std::string& envVar) {
    if (!envVar.empty()) {
        const char* v = std::getenv(envVar.c_str());
        if (v && *v) return v;
    }
    return "";
}
} // namespace

OpenAICompatibleProvider::OpenAICompatibleProvider(Settings s)
    : settings_(std::move(s)) {}

OpenAICompatibleProvider::~OpenAICompatibleProvider() = default;

bool OpenAICompatibleProvider::available() const {
    if (settings_.baseUrl.empty() || settings_.model.empty()) return false;
    std::string key = apiKeyOverride_.empty() ? resolveKey(settings_.apiKeyEnvVar)
                                              : apiKeyOverride_;
    return !key.empty();
}

CompletionResult OpenAICompatibleProvider::complete(const CompletionRequest& req) {
    CompletionResult res;
    if (!available()) {
        res.error = "provider not configured (base url / model / api key)";
        return res;
    }
    std::string key = apiKeyOverride_.empty() ? resolveKey(settings_.apiKeyEnvVar)
                                              : apiKeyOverride_;

    nlohmann::json body;
    body["model"] = req.model.empty() ? settings_.model : req.model;
    body["temperature"] = req.temperature > 0 ? req.temperature : settings_.temperature;
    body["max_tokens"] = req.maxTokens > 0 ? req.maxTokens : settings_.maxTokens;
    auto& msgs = body["messages"] = nlohmann::json::array();
    for (const auto& m : req.messages) {
        msgs.push_back({{"role", m.role}, {"content", m.content}});
    }

    HttpRequest http;
    http.method = "POST";
    std::string base = settings_.baseUrl;
    if (!base.empty() && base.back() == '/') base.pop_back();
    http.url = base + "/chat/completions";
    http.headers = {{"Authorization", "Bearer " + key},
                    {"Content-Type", "application/json"}};
    http.body = body.dump();
    double timeoutSec = req.timeoutSeconds > 0 ? req.timeoutSeconds
                                               : settings_.timeoutSeconds;
    http.timeout = std::chrono::milliseconds(
        static_cast<int64_t>(timeoutSec * 1000.0));

    HttpClient client;
    auto start = std::chrono::steady_clock::now();
    auto resp = client.request(http);
    res.latencyMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    if (resp.transportError || !resp.ok()) {
        res.error = "LLM HTTP error: " + std::to_string(resp.status) + " " +
                    resp.error;
        return res;
    }
    try {
        auto j = nlohmann::json::parse(resp.body);
        const auto& choices = j["choices"];
        if (!choices.empty() && choices[0].contains("message")) {
            res.content = choices[0]["message"].value("content", "");
        }
        if (j.contains("usage")) {
            res.promptTokens = j["usage"].value("prompt_tokens", 0);
            res.completionTokens = j["usage"].value("completion_tokens", 0);
        }
        res.ok = !res.content.empty();
        if (!res.ok) res.error = "empty completion";
    } catch (const std::exception& e) {
        res.error = std::string("LLM response parse failed: ") + e.what();
    }
    return res;
}

} // namespace trading
