#pragma once

// trading/connectors/ConnectorRegistry.hpp — creates and tracks connectors by
// logical name + environment (spec §8, §54). Connectors are created through a
// factory so the rest of the system depends only on ITradingConnector.

#include "trading/connectors/ITradingConnector.hpp"
#include "trading/core/Types.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace trading {

// Connector configuration passed at creation. Carries no secrets — those are
// resolved from the CredentialStore by the connector itself.
struct ConnectorConfig {
    std::string name;            // "alpaca"
    EnvMode env = EnvMode::PAPER;
    std::string label;           // UI label
    std::string accountId;       // optional broker account selector
    std::map<std::string, std::string> options;  // region, endpoints, etc.
};

class ConnectorRegistry {
public:
    using Factory = std::function<TradingConnectorPtr(const ConnectorConfig&)>;

    static ConnectorRegistry& instance() {
        static ConnectorRegistry reg;
        return reg;
    }

    // Registers a connector type factory. Idempotent (last wins).
    void registerFactory(const std::string& type, Factory factory);

    // Creates a connector (throws std::runtime_error on unknown type).
    TradingConnectorPtr create(const std::string& type, const ConnectorConfig& cfg);

    // Returns an existing connected connector by name+env, if present.
    std::optional<TradingConnectorPtr> find(const std::string& name,
                                            EnvMode env) const;

    // Track/untrack instances.
    void track(const TradingConnectorPtr& conn);
    void untrack(const std::string& name, EnvMode env);

    std::vector<std::string> supportedTypes() const;

private:
    ConnectorRegistry() = default;

    std::map<std::string, Factory> factories_;
    std::map<std::string, TradingConnectorPtr> instances_;
    mutable std::mutex mutex_;
};

} // namespace trading
