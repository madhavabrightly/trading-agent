#pragma once

// trading/net/WebSocketClient.hpp — minimal RFC 6455 WebSocket client over a
// raw socket (TLS via OpenSSL). Pattern derived from the existing CDP
// RawWebSocket in the EdgeMonitor codebase. Supports text/binary frames and
// ping/pong; TLS certificate verification is always ON (spec §36).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace trading {

class WebSocketClient {
public:
    using MessageHandler = std::function<void(int opcode, const std::string& payload)>;
    using CloseHandler = std::function<void(const std::string& reason)>;

    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // url: ws://host[:port]/path or wss://host[:port]/path.
    // Blocks up to timeoutMs for connect + HTTP upgrade + (for wss) TLS.
    bool connect(const std::string& url, int timeoutMs = 10000,
                 std::string* errorOut = nullptr);

    void setMessageHandler(MessageHandler h) { msgHandler_ = std::move(h); }
    void setCloseHandler(CloseHandler h) { closeHandler_ = std::move(h); }

    bool sendText(const std::string& text);
    bool sendPing(const std::string& payload = "");

    bool isConnected() const { return connected_.load(); }

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> connected_{false};
    MessageHandler msgHandler_;
    CloseHandler closeHandler_;
};

} // namespace trading
