#pragma once

#include "core/Types.hpp"
#include "edge/CDPClient.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <atomic>
#include <functional>
#include <chrono>
#include <optional>

namespace edgemon {

class PageSession : public std::enable_shared_from_this<PageSession> {
public:
    using DOMCallback = std::function<void(const std::string& text)>;
    using NavigationCallback = std::function<void(const std::string& url, const std::string& title)>;
    using StateCallback = std::function<void(TargetState state, const std::string& message)>;
    using ErrorCallback = std::function<void(const std::string& error, bool recoverable)>;

    PageSession(TargetId targetId, CDPTargetId cdpTargetId, std::string webSocketUrl,
                std::shared_ptr<ThreadPool> pool);
    ~PageSession();

    PageSession(const PageSession&) = delete;
    PageSession& operator=(const PageSession&) = delete;

    bool connect();
    void disconnect();

    bool isConnected() const;
    TargetState getState() const { return state_.load(); }
    TargetId getTargetId() const { return targetId_; }
    CDPTargetId getCDPTargetId() const { return cdpTargetId_; }

    void enableDOMMonitoring();
    void enablePageEvents();
    void enableRuntimeEvents();
    void enableNetworkEvents();

    std::string executeJavaScript(const std::string& script);
    std::optional<nlohmann::json> executeJavaScriptSync(const std::string& script,
                                                       std::chrono::milliseconds timeout = std::chrono::seconds(5));

    std::string getDocumentContent();
    nlohmann::json getDOMDocument();
    
    nlohmann::json captureScreenshot(const std::string& format = "png",
                                     int quality = 90,
                                     bool fullPage = false);

    void setDOMCallback(DOMCallback callback);
    void setNavigationCallback(NavigationCallback callback);
    void setStateCallback(StateCallback callback);
    void setErrorCallback(ErrorCallback callback);

    void setReconnectPolicy(int maxRetries = 5,
                           std::chrono::milliseconds initialDelay = std::chrono::milliseconds(250),
                           std::chrono::milliseconds maxDelay = std::chrono::seconds(30));

private:
    void initializeSession();
    void setupEventHandlers();
    void handleDisconnect();
    void attemptReconnect();
    void onNavigationEvent(const nlohmann::json& params);
    void onDOMContentChanged(const nlohmann::json& params);
    void onLoadEvent(const nlohmann::json& params);
    void notifyStateChange();
    
    TargetId targetId_;
    CDPTargetId cdpTargetId_;
    std::string webSocketUrl_;
    
    std::shared_ptr<ThreadPool> pool_;
    std::shared_ptr<CDPClient> client_;
    
    std::atomic<TargetState> state_;
    std::string currentUrl_;
    std::string currentTitle_;
    
    DOMCallback domCallback_;
    NavigationCallback navigationCallback_;
    StateCallback stateCallback_;
    ErrorCallback errorCallback_;
    
    std::atomic<bool> reconnecting_;
    std::atomic<bool> running_;
    
    int maxRetries_;
    std::chrono::milliseconds initialDelay_;
    std::chrono::milliseconds maxDelay_;
    int retryCount_;
    
    std::mutex reconnectMutex_;
    std::jthread reconnectThread_;
};

} // namespace edgemon
