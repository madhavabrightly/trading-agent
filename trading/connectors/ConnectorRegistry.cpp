#include "trading/connectors/ConnectorRegistry.hpp"
#include "logger.hpp"

#include <stdexcept>

namespace trading {

void ConnectorRegistry::registerFactory(const std::string& type,
                                        Factory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    factories_[type] = std::move(factory);
}

TradingConnectorPtr ConnectorRegistry::create(const std::string& type,
                                              const ConnectorConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = factories_.find(type);
    if (it == factories_.end())
        throw std::runtime_error("unknown connector type: " + type);
    return it->second(cfg);
}

std::optional<TradingConnectorPtr> ConnectorRegistry::find(
    const std::string& name, EnvMode env) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = name + "\x1F" + toString(env);
    auto it = instances_.find(key);
    if (it == instances_.end()) return std::nullopt;
    return it->second;
}

void ConnectorRegistry::track(const TradingConnectorPtr& conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> lock(mutex_);
    instances_[conn->name() + "\x1F" + toString(conn->environment())] = conn;
}

void ConnectorRegistry::untrack(const std::string& name, EnvMode env) {
    std::lock_guard<std::mutex> lock(mutex_);
    instances_.erase(name + "\x1F" + toString(env));
}

std::vector<std::string> ConnectorRegistry::supportedTypes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(factories_.size());
    for (const auto& [k, v] : factories_) out.push_back(k);
    return out;
}

} // namespace trading
