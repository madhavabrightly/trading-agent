#pragma once

// trading/ai/AiClient.hpp — the C++ face of the AI layer. Combines an
// ILLMProvider (direct OpenAI-compatible endpoint) with an optional local
// Python service (FastAPI) for RAG/embeddings/structured extraction.
// The AI recommends; it never places orders (spec §13, §56).

#include "trading/ai/ILLMProvider.hpp"
#include "trading/ocr/OcrTypes.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trading {

// Structured AI verdict on a piece of text/context (spec §13 output shape).
struct AiAnalysis {
    bool ok = false;
    std::string error;

    std::string type;                 // news | economic | alert | chart | ...
    std::string asset;                // canonical asset when detected
    double sentiment = 0.0;           // -1..1
    double confidence = 0.0;          // 0..1
    std::string urgency;              // low | medium | high
    std::string summary;              // 1-2 sentence human summary
    std::string raw;                  // raw model text (trimmed)
};

// What the AI layer exposes to strategies.
class AiClient {
public:
    // Uses the given provider. When pythonServiceUrl is non-empty the client
    // will first try the local Python service, then fall back to the provider.
    explicit AiClient(std::shared_ptr<ILLMProvider> provider,
                      std::string pythonServiceUrl = "");

    // Analyze OCR/news text into structured info. Never blocks forever; returns
    // ok=false on any timeout/error so the pipeline degrades safely.
    AiAnalysis analyze(const OcrEvent& event, double timeoutSec = 20.0);

    // Analyze a raw text blob (news headline, alert, chart text).
    AiAnalysis analyzeText(const std::string& text, const std::string& role,
                           double timeoutSec = 20.0);

    // RAG query against the Python service (optional). Returns retrieved
    // context passages joined by "\n---\n".
    std::vector<std::string> ragQuery(const std::string& question,
                                      int topK = 5, double timeoutSec = 15.0);

    // Health of the Python service (used by the setup UI [TEST]).
    bool pythonServiceHealthy(double timeoutSec = 3.0) const;

    bool providerAvailable() const { return provider_ && provider_->available(); }
    bool degraded() const { return !providerAvailable() && !pythonServiceHealthy(1.0); }

private:
    std::shared_ptr<ILLMProvider> provider_;
    std::string pythonServiceUrl_;
};

} // namespace trading
