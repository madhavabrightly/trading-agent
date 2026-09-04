#include "trading/ai/AiClient.hpp"
#include "trading/net/HttpClient.hpp"
#include "logger.hpp"

#include <nlohmann/json.hpp>
#include <chrono>

namespace trading {

namespace {

std::string buildPrompt(const std::string& text, const std::string& role) {
    return std::string(
               "You are a market-data analysis engine. Classify the following "
               "OCR text captured from a trading-related page (role: ") +
           role +
           "). Respond with STRICT JSON only, no markdown, no prose:\n"
           "{\"type\":\"news|economic|alert|price|chart|platform|unknown\","
           "\"asset\":\"CANONICAL_SYMBOL_OR_EMPTY\",\"sentiment\":-1.0,"
           "\"confidence\":0.0,\"urgency\":\"low|medium|high\","
           "\"summary\":\"one short sentence\"}\n"
           "Rules: sentiment in [-1,1]; asset like BTC/USD or AAPL or empty; "
           "confidence reflects how sure you are the text is market-relevant. "
           "Never invent facts not in the text.\n"
           "TEXT: " +
           text;
}

double clampJsonDouble(const nlohmann::json& j, const char* key, double lo,
                       double hi, double dflt) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return dflt;
    double v = it->get<double>();
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

// Parse a model JSON reply defensively (models wrap in ``` fences etc).
nlohmann::json extractJsonObject(const std::string& text) {
    std::string t = text;
    auto codeFence = t.find("```");
    if (codeFence != std::string::npos) {
        auto end = t.find("```", codeFence + 3);
        if (end != std::string::npos) {
            t = t.substr(codeFence + 3, end - codeFence - 3);
        } else {
            t = t.substr(codeFence + 3);
        }
    }
    auto brace = t.find('{');
    if (brace == std::string::npos) return nlohmann::json::object();
    auto close = t.rfind('}');
    if (close == std::string::npos || close < brace)
        return nlohmann::json::object();
    try {
        return nlohmann::json::parse(t.substr(brace, close - brace + 1));
    } catch (...) {
        return nlohmann::json::object();
    }
}

} // namespace

AiClient::AiClient(std::shared_ptr<ILLMProvider> provider,
                   std::string pythonServiceUrl)
    : provider_(std::move(provider)),
      pythonServiceUrl_(std::move(pythonServiceUrl)) {}

AiAnalysis AiClient::analyzeText(const std::string& text, const std::string& role,
                                 double timeoutSec) {
    AiAnalysis out;
    if (!provider_) {
        out.error = "no LLM provider configured";
        return out;
    }
    CompletionRequest req;
    req.messages = {
        {"system", "You return strict JSON analysis of market text."},
        {"user", buildPrompt(text, role)}};
    req.timeoutSeconds = timeoutSec;
    auto res = provider_->complete(req);
    if (!res.ok) {
        out.error = res.error;
        return out;
    }
    out.raw = res.content;
    auto j = extractJsonObject(res.content);
    if (j.empty()) {
        out.error = "model returned no parseable JSON";
        return out;
    }
    out.ok = true;
    out.type = j.value("type", "unknown");
    out.asset = j.value("asset", "");
    out.sentiment = clampJsonDouble(j, "sentiment", -1.0, 1.0, 0.0);
    out.confidence = clampJsonDouble(j, "confidence", 0.0, 1.0, 0.0);
    out.urgency = j.value("urgency", "low");
    out.summary = j.value("summary", "");
    return out;
}

AiAnalysis AiClient::analyze(const OcrEvent& event, double timeoutSec) {
    AiAnalysis out;
    // Prefer the local Python service when healthy (it can do RAG + extraction
    // + embeddings in one hop); fall back to the direct provider.
    if (!pythonServiceUrl_.empty()) {
        HttpClient http;
        HttpRequest req;
        req.method = "POST";
        std::string base = pythonServiceUrl_;
        if (!base.empty() && base.back() == '/') base.pop_back();
        req.url = base + "/analyze";
        req.headers["Content-Type"] = "application/json";
        nlohmann::json body;
        body["text"] = event.text;
        body["role"] = event.kind.empty() ? "news" : event.kind;
        body["source"] = event.source;
        req.body = body.dump();
        req.timeout = std::chrono::milliseconds(
            static_cast<int64_t>(timeoutSec * 1000.0));
        auto resp = http.request(req);
        if (!resp.transportError && resp.ok()) {
            try {
                auto j = nlohmann::json::parse(resp.body);
                out.ok = j.value("ok", false);
                out.type = j.value("type", "unknown");
                out.asset = j.value("asset", "");
                out.sentiment = clampJsonDouble(j, "sentiment", -1.0, 1.0, 0.0);
                out.confidence = clampJsonDouble(j, "confidence", 0.0, 1.0, 0.0);
                out.urgency = j.value("urgency", "low");
                out.summary = j.value("summary", "");
                out.raw = j.value("raw", "");
                out.error = j.value("error", "");
                return out;
            } catch (const std::exception&) {
                // fall through to provider
            }
        }
        LOG_DEBUG("AiClient: python service unavailable, falling back to provider");
    }
    return analyzeText(event.text, event.kind.empty() ? "news" : event.kind,
                       timeoutSec);
}

std::vector<std::string> AiClient::ragQuery(const std::string& question,
                                            int topK, double timeoutSec) {
    std::vector<std::string> out;
    if (pythonServiceUrl_.empty()) return out;
    HttpClient http;
    HttpRequest req;
    req.method = "POST";
    std::string base = pythonServiceUrl_;
    if (!base.empty() && base.back() == '/') base.pop_back();
    req.url = base + "/rag/query";
    req.headers["Content-Type"] = "application/json";
    nlohmann::json body;
    body["question"] = question;
    body["top_k"] = topK;
    req.body = body.dump();
    req.timeout = std::chrono::milliseconds(
        static_cast<int64_t>(timeoutSec * 1000.0));
    auto resp = http.request(req);
    if (!resp.ok()) return out;
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (j.contains("passages") && j["passages"].is_array()) {
            for (const auto& p : j["passages"]) {
                if (p.is_string()) out.push_back(p.get<std::string>());
            }
        }
    } catch (...) {}
    return out;
}

bool AiClient::pythonServiceHealthy(double timeoutSec) const {
    if (pythonServiceUrl_.empty()) return false;
    HttpClient http;
    std::string base = pythonServiceUrl_;
    if (!base.empty() && base.back() == '/') base.pop_back();
    auto resp = http.get(base + "/health", {},
                         std::chrono::milliseconds(
                             static_cast<int64_t>(timeoutSec * 1000.0)));
    if (!resp.ok() || resp.transportError) return false;
    try {
        auto j = nlohmann::json::parse(resp.body);
        return j.value("status", "") == "ok";
    } catch (...) {
        return false;
    }
}

} // namespace trading
