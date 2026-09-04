#include "edge/PageSession.hpp"
#include "logger.hpp"

namespace edgemon {

PageSession::PageSession(TargetId targetId, CDPTargetId cdpTargetId, std::string webSocketUrl,
                        std::shared_ptr<ThreadPool> pool)
    : targetId_(std::move(targetId))
    , cdpTargetId_(std::move(cdpTargetId))
    , webSocketUrl_(std::move(webSocketUrl))
    , pool_(std::move(pool))
    , state_(TargetState::Discovered)
    , reconnecting_(false)
    , running_(false)
    , maxRetries_(5)
    , initialDelay_(std::chrono::milliseconds(250))
    , maxDelay_(std::chrono::seconds(30))
    , retryCount_(0) {
}

PageSession::~PageSession() {
    disconnect();
}

bool PageSession::connect() {
    if (running_.exchange(true)) {
        LOG_WARN("PageSession {} already running", targetId_.value);
        return false;
    }

    state_ = TargetState::Connecting;
    notifyStateChange();

    client_ = std::make_shared<CDPClient>(pool_);

    client_->setOnDisconnect([this]() {
        handleDisconnect();
    });

    client_->connect(webSocketUrl_, [this](bool success, const std::string& error) {
        if (success) {
            state_ = TargetState::Connected;
            notifyStateChange();
            initializeSession();
        } else {
            state_ = TargetState::Error;
            notifyStateChange();
            LOG_ERROR("PageSession {} connection failed: {}", targetId_.value, error);
            
            if (errorCallback_) {
                errorCallback_(error, true);
            }
        }
    });

    LOG_INFO("PageSession {} connecting to {}", targetId_.value, webSocketUrl_);
    return true;
}

void PageSession::disconnect() {
    if (!running_.exchange(false)) {
        return;
    }

    reconnecting_ = false;
    if (reconnectThread_.joinable()) {
        reconnectThread_.request_stop();
        reconnectThread_.join();
    }

    if (client_) {
        client_->disconnect();
        client_.reset();
    }

    state_ = TargetState::Closed;
    notifyStateChange();
    
    LOG_INFO("PageSession {} disconnected", targetId_.value);
}

bool PageSession::isConnected() const {
    return client_ && client_->isConnected();
}

void PageSession::initializeSession() {
    if (!client_ || !client_->isConnected()) {
        LOG_ERROR("PageSession {} cannot initialize: not connected", targetId_.value);
        return;
    }

    enablePageEvents();
    enableDOMMonitoring();
    enableRuntimeEvents();
    enableNetworkEvents();

    auto docResult = client_->sendCommandSync("DOM.getDocument", {}, std::chrono::seconds(5));
    if (docResult) {
        LOG_DEBUG("PageSession {} DOM initialized", targetId_.value);
    }

    auto urlResult = client_->sendCommandSync("Page.getNavigationHistory", {}, std::chrono::seconds(5));
    
    state_ = TargetState::Monitoring;
    notifyStateChange();
    
    LOG_INFO("PageSession {} monitoring started", targetId_.value);
}

void PageSession::enableDOMMonitoring() {
    if (!client_) return;

    client_->sendCommand("DOM.enable");
    
    client_->subscribe("DOM.documentUpdated", [this](const std::string& method, const nlohmann::json& params) {
        onDOMContentChanged(params);
    });

    client_->subscribe("DOM.childNodesUpdated", [this](const std::string& method, const nlohmann::json& params) {
        pool_->submit([this, params]() {
            onDOMContentChanged(params);
        });
    });

    client_->subscribe("DOM.characterDataModified", [this](const std::string& method, const nlohmann::json& params) {
        pool_->submit([this, params]() {
            onDOMContentChanged(params);
        });
    });

    LOG_DEBUG("PageSession {} DOM monitoring enabled", targetId_.value);
}

void PageSession::enablePageEvents() {
    if (!client_) return;

    client_->sendCommand("Page.enable");

    client_->subscribe("Page.frameNavigated", [this](const std::string& method, const nlohmann::json& params) {
        pool_->submit([this, params]() {
            onNavigationEvent(params);
        });
    });

    client_->subscribe("Page.loadEventFired", [this](const std::string& method, const nlohmann::json& params) {
        pool_->submit([this, params]() {
            onLoadEvent(params);
        });
    });

    client_->subscribe("Page.navigatedWithinDocument", [this](const std::string& method, const nlohmann::json& params) {
        pool_->submit([this, params]() {
            onNavigationEvent(params);
        });
    });

    LOG_DEBUG("PageSession {} page events enabled", targetId_.value);
}

void PageSession::enableRuntimeEvents() {
    if (!client_) return;

    client_->sendCommand("Runtime.enable");

    client_->subscribe("Runtime.executionContextCreated", [this](const std::string& method, const nlohmann::json& params) {
        LOG_DEBUG("PageSession {}: execution context created", targetId_.value);
    });

    client_->subscribe("Runtime.executionContextDestroyed", [this](const std::string& method, const nlohmann::json& params) {
        LOG_DEBUG("PageSession {}: execution context destroyed", targetId_.value);
    });

    LOG_DEBUG("PageSession {} runtime events enabled", targetId_.value);
}

void PageSession::enableNetworkEvents() {
    if (!client_) return;

    client_->sendCommand("Network.enable");
    LOG_DEBUG("PageSession {} network events enabled", targetId_.value);
}

std::string PageSession::executeJavaScript(const std::string& script) {
    if (!client_ || !client_->isConnected()) {
        LOG_ERROR("PageSession {} cannot execute: not connected", targetId_.value);
        return "";
    }

    nlohmann::json params;
    params["expression"] = script;
    params["returnByValue"] = true;
    params["awaitPromise"] = true;

    std::string result;
    client_->sendCommand("Runtime.evaluate", params,
        [&result](const nlohmann::json& response) {
            if (response.contains("result")) {
                auto& resultObj = response["result"];
                if (resultObj.contains("value")) {
                    result = resultObj["value"].get<std::string>();
                } else if (resultObj.contains("description")) {
                    result = resultObj["description"].get<std::string>();
                }
            }
        });

    return result;
}

std::optional<nlohmann::json> PageSession::executeJavaScriptSync(const std::string& script,
                                                                 std::chrono::milliseconds timeout) {
    if (!client_ || !client_->isConnected()) {
        return std::nullopt;
    }

    nlohmann::json params;
    params["expression"] = script;
    params["returnByValue"] = true;
    params["awaitPromise"] = true;

    return client_->sendCommandSync("Runtime.evaluate", params, timeout);
}

std::string PageSession::getDocumentContent() {
    auto result = executeJavaScriptSync(R"(
        (function() {
            if (document.body) {
                return document.body.innerText || document.body.textContent || '';
            }
            return '';
        })()
    )", std::chrono::seconds(5));

    if (!result) return "";

    try {
        if (result->contains("result") && (*result)["result"].contains("value")) {
            auto& value = (*result)["result"]["value"];
            if (value.is_string()) {
                return value.get<std::string>();
            }
            if (value.is_null()) {
                // expression returned undefined; try description fallback
                if ((*result)["result"].contains("description")) {
                    return (*result)["result"]["description"].get<std::string>();
                }
            }
        }
    } catch (const std::exception&) {
        // fall through
    }

    return "";
}

nlohmann::json PageSession::getDOMDocument() {
    if (!client_ || !client_->isConnected()) {
        return nlohmann::json::object();
    }

    auto result = client_->sendCommandSync("DOM.getDocument", {}, std::chrono::seconds(5));
    return result.value_or(nlohmann::json::object());
}

nlohmann::json PageSession::captureScreenshot(const std::string& format, int quality, bool fullPage) {
    if (!client_ || !client_->isConnected()) {
        return nlohmann::json::object();
    }

    nlohmann::json params;
    params["format"] = format;
    params["quality"] = quality;
    params["fullPage"] = fullPage;

    auto result = client_->sendCommandSync("Page.captureScreenshot", params, std::chrono::seconds(10));
    return result.value_or(nlohmann::json::object());
}

void PageSession::setDOMCallback(DOMCallback callback) {
    domCallback_ = std::move(callback);
}

void PageSession::setNavigationCallback(NavigationCallback callback) {
    navigationCallback_ = std::move(callback);
}

void PageSession::setStateCallback(StateCallback callback) {
    stateCallback_ = std::move(callback);
}

void PageSession::setErrorCallback(ErrorCallback callback) {
    errorCallback_ = std::move(callback);
}

void PageSession::setReconnectPolicy(int maxRetries,
                                     std::chrono::milliseconds initialDelay,
                                     std::chrono::milliseconds maxDelay) {
    maxRetries_ = maxRetries;
    initialDelay_ = initialDelay;
    maxDelay_ = maxDelay;
}

void PageSession::handleDisconnect() {
    if (!running_) return;

    LOG_WARN("PageSession {} disconnected", targetId_.value);
    
    state_ = TargetState::Offline;
    notifyStateChange();

    attemptReconnect();
}

void PageSession::attemptReconnect() {
    if (reconnecting_.exchange(true)) {
        return;
    }

    if (retryCount_ >= maxRetries_) {
        LOG_ERROR("PageSession {} max reconnection attempts reached", targetId_.value);
        state_ = TargetState::Error;
        notifyStateChange();
        
        if (errorCallback_) {
            errorCallback_("Max reconnection attempts reached", false);
        }
        
        reconnecting_ = false;
        return;
    }

    std::lock_guard<std::mutex> lock(reconnectMutex_);
    
    reconnectThread_ = std::jthread([this](std::stop_token token) {
        auto delay = initialDelay_;
        
        for (int i = retryCount_; i < maxRetries_; ++i) {
            if (token.stop_requested() || !running_) {
                break;
            }

            LOG_INFO("PageSession {} reconnection attempt {}/{}", 
                     targetId_.value, i + 1, maxRetries_);

            state_ = TargetState::Reconnecting;
            notifyStateChange();

            if (connect()) {
                retryCount_ = 0;
                reconnecting_ = false;
                LOG_INFO("PageSession {} reconnected successfully", targetId_.value);
                return;
            }

            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, maxDelay_);
        }

        retryCount_++;
        reconnecting_ = false;
        
        state_ = TargetState::Offline;
        notifyStateChange();
    });
}

void PageSession::onNavigationEvent(const nlohmann::json& params) {
    std::string newUrl;
    std::string newTitle;

    if (params.contains("frame") && params["frame"].contains("url")) {
        newUrl = params["frame"]["url"].get<std::string>();
    } else if (params.contains("url")) {
        newUrl = params["url"].get<std::string>();
    }

    if (params.contains("frame") && params["frame"].contains("name")) {
        newTitle = params["frame"]["name"].get<std::string>();
    }

    if (!newUrl.empty() && newUrl != currentUrl_) {
        state_ = TargetState::Navigating;
        notifyStateChange();

        std::string oldUrl = currentUrl_;
        currentUrl_ = newUrl;
        currentTitle_ = newTitle;

        LOG_INFO("PageSession {} navigated: {} -> {}", 
                 targetId_.value, oldUrl, newUrl);

        if (navigationCallback_) {
            navigationCallback_(newUrl, newTitle);
        }

        state_ = TargetState::Monitoring;
        notifyStateChange();
    }
}

void PageSession::onDOMContentChanged(const nlohmann::json& params) {
    auto text = getDocumentContent();
    
    if (!text.empty() && domCallback_) {
        domCallback_(text);
    }
}

void PageSession::onLoadEvent(const nlohmann::json& params) {
    LOG_DEBUG("PageSession {} page loaded", targetId_.value);
    
    auto text = getDocumentContent();
    if (!text.empty() && domCallback_) {
        domCallback_(text);
    }
}

void PageSession::notifyStateChange() {
    if (stateCallback_) {
        std::string message;
        switch (state_.load()) {
            case TargetState::Connected:
                message = "Connected";
                break;
            case TargetState::Monitoring:
                message = "Monitoring";
                break;
            case TargetState::Navigating:
                message = "Navigating";
                break;
            case TargetState::Reconnecting:
                message = "Reconnecting...";
                break;
            case TargetState::Offline:
                message = "Offline";
                break;
            case TargetState::Error:
                message = "Error";
                break;
            default:
                message = TargetStateToString(state_.load());
        }
        
        stateCallback_(state_.load(), message);
    }
}

} // namespace edgemon
