#pragma once

// trading/connectors/alpaca/AlpacaConnector.hpp — first full connector (spec
// §54): authentication, account, positions, orders, cancel, market data
// (stream v2), reconciliation inputs, rate limiting, API-level health check.
// Official Alpaca REST + streaming API only. Paper by default; LIVE only when
// explicitly configured with live auth.

#include "trading/connectors/ITradingConnector.hpp"
#include "trading/connectors/ConnectorRegistry.hpp"
#include "trading/net/HttpClient.hpp"
#include "trading/net/WebSocketClient.hpp"
#include "trading/core/security/CredentialStore.hpp"
#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trading {

class AlpacaConnector : public ITradingConnector {
public:
    // cfg.env selects paper-api vs api host. Credentials come from the
    // CredentialStore (env vars or encrypted file), never from config.
    explicit AlpacaConnector(const ConnectorConfig& cfg);
    ~AlpacaConnector() override;

    // --- ITradingConnector ---
    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool authenticate() override;
    bool isAuthenticated() const override;

    std::string name() const override { return "alpaca"; }
    EnvMode environment() const override { return env_; }

    AccountInfo getAccount() override;
    std::vector<Position> getPositions() override;
    std::vector<Order> getOpenOrders() override;

    OrderResult placeOrder(const OrderRequest& request) override;
    CancelResult cancelOrder(const std::string& orderId) override;
    OrderResult modifyOrder(const std::string& orderId,
                            const OrderModification& mod) override;

    MarketSnapshot getMarketSnapshot(const std::string& symbol) override;
    void subscribeMarketData(const SubscriptionRequest& request) override;
    void unsubscribeMarketData(const std::string& symbol) override;
    void setTickCallback(TickCallback cb) override { tickCb_ = std::move(cb); }

    const ConnectorCapabilities& capabilities() const override {
        return capabilities_;
    }

    ConnectorHealth testConnection() override;

    // Test hook: manual creds (used by credential tests without env).
    void setCredentials(const std::string& apiKey, const std::string& apiSecret);

private:
    // REST helpers -----------------------------------------------------------
    HttpResponse get(const std::string& path);
    HttpResponse post(const std::string& path, const nlohmann::json& body);
    HttpResponse del(const std::string& path);
    std::string apiBase() const;
    std::string streamUrl() const;
    std::map<std::string, std::string> authHeaders() const;

    // Mapping helpers --------------------------------------------------------
    AccountInfo parseAccount(const nlohmann::json& j) const;
    Position parsePosition(const nlohmann::json& j) const;
    Order parseOrder(const nlohmann::json& j) const;
    OrderStatus mapOrderStatus(const std::string& s) const;
    std::string canonicalSymbol(const std::string& native) const;
    std::string nativeSymbol(const std::string& canonical) const;

    // Market stream ----------------------------------------------------------
    void streamLoop();
    void handleStreamMessage(const std::string& text);

    bool loadCredentials(std::string& key, std::string& secret);

    ConnectorConfig cfg_;
    EnvMode env_;
    HttpClient http_;

    std::string apiKey_;
    std::string apiSecret_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> authenticated_{false};

    std::shared_ptr<WebSocketClient> ws_;
    std::thread streamThread_;
    std::atomic<bool> streamStop_{true};
    std::mutex wsMutex_;

    TickCallback tickCb_;
    ConnectorCapabilities capabilities_;
    std::vector<std::string> subscribedSymbols_;

    // Last-known snapshot cache (used by getMarketSnapshot without a live sub).
    mutable std::mutex snapMutex_;
    std::map<std::string, MarketSnapshot> snapshots_;
};

} // namespace trading
