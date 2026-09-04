#pragma once

// trading/api/ControlApiServer.hpp — loopback-only control API (spec §42, §43).
// Exposes /health /status /connectors /connect /disconnect /positions /orders
// /signals /risk /kill-switch over HTTP, plus a WebSocket event stream that
// mirrors the trading event bus. Binds 127.0.0.1 by default; never exposed
// publicly. Auth via bearer token when configured.

#include "trading/core/event/TradingEventBus.hpp"
#include "trading/connectors/ConnectorRegistry.hpp"
#include "trading/core/config/ConfigStore.hpp"
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trading {

// Delegate interface the server calls to act on the trading core. The app
// (Setup Terminal / headless runner) implements it; the API server stays
// decoupled from the concrete engine.
class IControlBackend {
public:
    virtual ~IControlBackend() = default;

    virtual nlohmann::json status() = 0;
    virtual nlohmann::json connectors() = 0;
    virtual nlohmann::json connect(const std::string& type,
                                   const std::string& env,
                                   const nlohmann::json& options) = 0;
    virtual nlohmann::json disconnect(const std::string& name) = 0;
    virtual nlohmann::json positions() = 0;
    virtual nlohmann::json orders() = 0;
    virtual nlohmann::json placeOrder(const nlohmann::json& req) = 0;
    virtual nlohmann::json cancelOrder(const std::string& orderId) = 0;
    virtual nlohmann::json signals() = 0;
    virtual nlohmann::json risk() = 0;
    virtual nlohmann::json killSwitch(bool engage) = 0;
};

class ControlApiServer {
public:
    ControlApiServer(IControlBackend& backend, const ApiConfig& cfg);
    ~ControlApiServer();

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }
    uint16_t port() const { return cfg_.port; }

    // Pushes an event to all connected WebSocket clients (JSON text frames).
    void broadcast(const BusEvent& event);

private:
    void acceptLoop();
    void handleClient(uintptr_t clientSock);
    void handleRequest(const std::string& method, const std::string& path,
                       const std::map<std::string, std::string>& headers,
                       const std::string& body, std::string& response,
                       std::string& contentType, bool& wsUpgrade,
                       std::string& wsAcceptKey);

    IControlBackend& backend_;
    ApiConfig cfg_;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;

    mutable std::mutex clientsMutex_;
    std::vector<uintptr_t> wsClients_;
};

} // namespace trading
