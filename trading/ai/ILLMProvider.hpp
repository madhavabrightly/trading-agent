#pragma once

// trading/ai/ILLMProvider.hpp — replaceable LLM provider (spec §16). The
// trading logic depends only on this interface; OpenAI-compatible endpoints,
// local models, and the Python AI service are all implementations.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trading {

// A single chat message in the provider's native format.
struct ChatMessage {
    std::string role;    // "system" | "user" | "assistant"
    std::string content;
};

// One model completion request.
struct CompletionRequest {
    std::string model;                     // provider model name
    std::vector<ChatMessage> messages;
    double temperature = 0.2;
    int maxTokens = 2048;
    double timeoutSeconds = 30.0;
};

struct CompletionResult {
    bool ok = false;
    std::string error;                     // never contains secrets
    std::string content;                   // assistant text
    int promptTokens = 0;
    int completionTokens = 0;
    uint64_t latencyMs = 0;
};

class ILLMProvider {
public:
    virtual ~ILLMProvider() = default;

    virtual std::string name() const = 0;   // "openai", "deepseek", "python-ai"
    virtual bool available() const = 0;     // false when misconfigured/down

    virtual CompletionResult complete(const CompletionRequest& req) = 0;
};

// OpenAI-compatible chat completions over HTTP. Base URL, model, key and
// timeout are configurable; the key is resolved from the credential store /
// environment, never logged.
class OpenAICompatibleProvider : public ILLMProvider {
public:
    struct Settings {
        std::string name = "openai-compatible";
        std::string baseUrl;         // e.g. https://api.openai.com/v1
        std::string model;
        std::string apiKeyEnvVar = "AI_API_KEY";
        double temperature = 0.2;
        int maxTokens = 2048;
        double timeoutSeconds = 30.0;
    };

    explicit OpenAICompatibleProvider(Settings s);
    ~OpenAICompatibleProvider() override;

    std::string name() const override { return settings_.name; }
    bool available() const override;

    CompletionResult complete(const CompletionRequest& req) override;

    // Lets tests inject a key without touching the environment.
    void setApiKey(const std::string& key) { apiKeyOverride_ = key; }

private:
    Settings settings_;
    std::string apiKeyOverride_;
};

} // namespace trading
