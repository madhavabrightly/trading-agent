#pragma once

#include "common.hpp"
#include "threadpool.hpp"
#include <string>
#include <map>
#include <functional>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <nlohmann/json.hpp>

namespace edgemon {

using CDPClientPtr = std::shared_ptr<class CDPClient>;
using CDPMessageCallback = std::function<void(const nlohmann::json&)>;

class CDPClient : public std::enable_shared_from_this<CDPClient> {
public:
    using MessageHandler = std::function<void(const nlohmann::json&)>;
    
    CDPClient(std::shared_ptr<ThreadPool> pool);
    ~CDPClient();

    void connect(const std::string& url, std::function<void(bool)> callback);
    void disconnect();

    int sendCommand(const std::string& method, const nlohmann::json& params = {});
    void sendCommand(const std::string& method, const nlohmann::json& params,
                     std::function<void(const nlohmann::json&)> callback);

    void subscribe(const std::string& event, MessageHandler handler);
    void unsubscribe(const std::string& event);

    bool isConnected() const { return connected_; }
    int getDebugPort() const { return debugPort_; }
    std::string getTargetId() const { return targetId_; }

    void enableDomain(const std::string& domain);

    nlohmann::json getDOMDocument();
    nlohmann::json getOuterHTML(const std::string& nodeId = "");
    std::string getDocumentContent();
    nlohmann::json captureScreenshot(const std::string& format = "png");

    void setOnDisconnect(std::function<void()> handler);

private:
    void onOpen(websocketpp::connection_hdl hdl);
    void onClose(websocketpp::connection_hdl hdl);
    void onMessage(websocketpp::connection_hdl hdl, 
                   websocketpp::config::asio_client::message_type::ptr msg);
    void handleMessage(const nlohmann::json& message);

    websocketpp::client<websocketpp::config::asio_client> client_;
    websocketpp::connection_hdl connection_;
    std::shared_ptr<ThreadPool> pool_;
    
    std::atomic<bool> connected_;
    int debugPort_ = 0;
    std::string targetId_;
    
    std::mutex messageMutex_;
    std::map<int, std::pair<std::string, std::function<void(const nlohmann::json&)>>> pendingCommands_;
    std::map<std::string, std::vector<MessageHandler>> eventHandlers_;
    std::atomic<int> messageId_;
    
    std::function<void()> onDisconnectHandler_;
};

struct CDPResponse {
    bool success;
    nlohmann::json result;
    std::string error;
    int id;
};

}
