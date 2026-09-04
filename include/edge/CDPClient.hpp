#pragma once

#include "core/Types.hpp"
#include "threadpool.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <optional>

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

namespace edgemon {

class CDPClient : public std::enable_shared_from_this<CDPClient> {
public:
    using MessageHandler = std::function<void(const std::string& method, const nlohmann::json& params)>;
    using ResponseCallback = std::function<void(const nlohmann::json& result)>;
    using ConnectionCallback = std::function<void(bool success, const std::string& error)>;
    using DisconnectCallback = std::function<void()>;

    CDPClient(std::shared_ptr<ThreadPool> pool);
    ~CDPClient();

    CDPClient(const CDPClient&) = delete;
    CDPClient& operator=(const CDPClient&) = delete;

    void connect(const std::string& webSocketUrl, ConnectionCallback callback = nullptr);
    void disconnect();

    bool isConnected() const { return connected_.load(); }
    const std::string& getWebSocketUrl() const { return webSocketUrl_; }

    int64_t sendCommand(const std::string& method, const nlohmann::json& params = {});
    
    void sendCommand(const std::string& method, const nlohmann::json& params,
                     ResponseCallback callback);
    
    std::optional<nlohmann::json> sendCommandSync(const std::string& method, 
                                                  const nlohmann::json& params = {},
                                                  std::chrono::milliseconds timeout = std::chrono::seconds(5));

    void subscribe(const std::string& event, MessageHandler handler);
    void unsubscribe(const std::string& event);

    void setRequestTimeout(std::chrono::milliseconds timeout) { requestTimeout_ = timeout; }
    void setOnDisconnect(DisconnectCallback callback) { onDisconnect_ = std::move(callback); }

    void enableDomain(const std::string& domain);

    struct CDPCommand {
        int64_t id;
        std::string method;
        nlohmann::json params;
        ResponseCallback callback;
        std::chrono::steady_clock::time_point sentAt;
    };

private:
    void onConnected();
    void onDisconnected(const std::string& reason);
    void onMessage(const std::string& payload);
    void handleResponse(const nlohmann::json& message);
    void handleEvent(const nlohmann::json& message);
    
    void processPendingRequests();
    void cleanupTimedOutRequests();

    std::shared_ptr<ThreadPool> pool_;
    
    std::atomic<bool> connected_;
    std::string webSocketUrl_;
    
    std::atomic<int64_t> nextMessageId_;
    
    std::mutex pendingMutex_;
    std::unordered_map<int64_t, CDPCommand> pendingCommands_;
    
    std::mutex handlersMutex_;
    std::unordered_map<std::string, std::vector<MessageHandler>> eventHandlers_;
    
    std::chrono::milliseconds requestTimeout_;
    DisconnectCallback onDisconnect_;
    
    std::atomic<bool> running_;
    std::jthread processingThread_;

    // Persistent WebSocket transport (WebSocketImpl is defined in the .cpp).
    std::mutex wsMutex_;
    std::shared_ptr<void> ws_;
};

nlohmann::json nlohmann_json_parse(const std::string& json);

} // namespace edgemon
