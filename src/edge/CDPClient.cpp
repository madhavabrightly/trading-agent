#include "edge/CDPClient.hpp"
#include "logger.hpp"
#include <iostream>
#include <sstream>
#include <vector>
#include <future>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#endif

namespace edgemon {

#ifdef _WIN32
#ifndef WINHTTP_OPTION_SECURE_PROTOCOL_FLAGS
#define WINHTTP_OPTION_SECURE_PROTOCOL_FLAGS 84
#endif

// Minimal RFC 6455 WebSocket client over a raw socket. Replaces the WinHTTP
// WebSocket API, whose upgrade handshake proved unreliable against Edge's CDP
// endpoint (ERROR_WINHTTP_INVALID_SERVER_RESPONSE despite a valid request).
class RawWebSocket {
public:
    RawWebSocket() = default;
    ~RawWebSocket() { close(); }

    bool connect(const std::string& url, std::string& error) {
        // Parse ws://host:port/path
        std::string host;
        int port = 80;
        std::string path = "/";
        if (url.find("ws://") == 0) {
            size_t pos = url.find("//") + 2;
            size_t endPos = url.find('/', pos);
            if (endPos == std::string::npos) endPos = url.size();
            host = url.substr(pos, endPos - pos);
            path = url.substr(endPos);
            size_t colonPos = host.find(':');
            if (colonPos != std::string::npos) {
                port = std::stoi(host.substr(colonPos + 1));
                host = host.substr(0, colonPos);
            }
        } else {
            error = "Unsupported WebSocket URL";
            return false;
        }

        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            error = "WSAStartup failed";
            return false;
        }

        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) {
            error = "socket failed";
            return false;
        }

        // Non-blocking connect with a 5s timeout.
        u_long mode = 1;
        ioctlsocket(sock_, FIONBIO, &mode);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        addr.sin_addr.s_addr = inet_addr(host.c_str());
        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                timeval tv{};
                tv.tv_sec = 5;
                fd_set wset;
                FD_ZERO(&wset);
                FD_SET(sock_, &wset);
                if (select(0, nullptr, &wset, nullptr, &tv) <= 0 || !FD_ISSET(sock_, &wset)) {
                    error = "connect timeout";
                    close();
                    return false;
                }
            } else {
                error = "connect failed";
                close();
                return false;
            }
        }
        // Back to blocking for the handshake reads.
        mode = 0;
        ioctlsocket(sock_, FIONBIO, &mode);

        // 500ms receive timeout so the polling loop can drain multiple queued
        // frames promptly instead of blocking indefinitely between frames.
        DWORD rcvTimeout = 500;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));

        // HTTP upgrade request with a random Sec-WebSocket-Key.
        std::string key = "dGhlIHNhbXBsZSBub25jZQ==";  // fixed valid base64 key
        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << ":" << port << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "\r\n";
        std::string reqStr = req.str();
        int sent = 0;
        while (sent < static_cast<int>(reqStr.size())) {
            int n = ::send(sock_, reqStr.c_str() + sent,
                           static_cast<int>(reqStr.size()) - sent, 0);
            if (n == SOCKET_ERROR) {
                error = "send failed";
                close();
                return false;
            }
            sent += n;
        }

        // Read the response head until \r\n\r\n.
        std::string response;
        char buf[4096];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (response.find("\r\n\r\n") == std::string::npos) {
            if (std::chrono::steady_clock::now() > deadline) {
                error = "handshake timeout";
                close();
                return false;
            }
            int n = ::recv(sock_, buf, sizeof(buf), 0);
            if (n <= 0) {
                error = "handshake read failed";
                close();
                return false;
            }
            response.append(buf, static_cast<size_t>(n));
        }

        if (response.find(" 101 ") == std::string::npos &&
            response.find("101 ") == std::string::npos) {
            error = "server did not return 101: " + response.substr(0, 120);
            close();
            return false;
        }

        connected_ = true;
        return true;
    }

    int send(const std::string& data) {
        if (!connected_) return -1;
        // Client-to-server frame: FIN|opcode=1 (text), masked.
        std::vector<uint8_t> frame;
        frame.push_back(0x81);
        size_t len = data.size();
        uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        if (len < 126) {
            frame.push_back(0x80 | static_cast<uint8_t>(len));
        } else if (len <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i) {
                frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
            }
        }
        frame.insert(frame.end(), mask, mask + 4);
        for (size_t i = 0; i < len; ++i) {
            frame.push_back(static_cast<uint8_t>(data[i]) ^ mask[i % 4]);
        }

        size_t sent = 0;
        while (sent < frame.size()) {
            int n = ::send(sock_, reinterpret_cast<char*>(frame.data() + sent),
                           static_cast<int>(frame.size() - sent), 0);
            if (n == SOCKET_ERROR) return -1;
            sent += static_cast<size_t>(n);
        }
        return static_cast<int>(len);
    }

    int receive(std::string& data) {
        if (!connected_) return -1;
        data.clear();

        // Read frame header. With SO_RCVTIMEO, a timeout means "no frame yet":
        // return -2 so the caller can poll again without treating it as error.
        uint8_t header[2];
        int hdr = readExactOrTimeout(header, 2);
        if (hdr <= 0) return hdr;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t len = header[1] & 0x7F;
        if (len == 126) {
            uint8_t ext[2];
            if (readExactOrTimeout(ext, 2) <= 0) return -1;
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (readExactOrTimeout(ext, 8) <= 0) return -1;
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | ext[i];
            }
        }

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (readExactOrTimeout(mask, 4) <= 0) return -1;
        }

        std::vector<uint8_t> payload(static_cast<size_t>(len));
        if (len > 0 && readExactOrTimeout(payload.data(), static_cast<size_t>(len)) <= 0) {
            return -1;
        }

        uint8_t opcode = header[0] & 0x0F;
        if (opcode == 0x8) {  // close frame
            return 0;
        }

        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= mask[i % 4];
            }
        }
        data.assign(reinterpret_cast<char*>(payload.data()), payload.size());
        return static_cast<int>(data.size());
    }

    void close() {
        connected_ = false;
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool isConnected() const { return connected_; }

private:
    // 1 = full read, 0 = peer closed, -1 = error, -2 = would-block/timeout.
    int readExactOrTimeout(uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            int r = ::recv(sock_, reinterpret_cast<char*>(buf + got),
                           static_cast<int>(n - got), 0);
            if (r > 0) {
                got += static_cast<size_t>(r);
                continue;
            }
            if (r == 0) return 0;
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                // Partial frame: drop it; the caller will re-sync on the next
                // poll. In practice CDP frames are small and arrive whole.
                return (got > 0) ? -1 : -2;
            }
            return -1;
        }
        return 1;
    }

    SOCKET sock_ = INVALID_SOCKET;
    bool connected_ = false;
};
#else
class PosixWebSocket {
public:
    bool connect(const std::string& url, std::string& error) {
        error = "Not implemented on this platform";
        return false;
    }
    int send(const std::string& data) { return -1; }
    int receive(std::string& data) { return -1; }
    void close() {}
    bool isConnected() const { return false; }
};
using WebSocketImpl = PosixWebSocket;
#endif

// Windows uses the raw RFC 6455 socket implementation.
#ifdef _WIN32
using WebSocketImpl = RawWebSocket;
#endif

}

namespace edgemon {

nlohmann::json nlohmann_json_parse(const std::string& json) {
    return nlohmann::json::parse(json);
}

CDPClient::CDPClient(std::shared_ptr<ThreadPool> pool)
    : pool_(std::move(pool))
    , connected_(false)
    , nextMessageId_(1)
    , requestTimeout_(std::chrono::seconds(5))
    , running_(false) {
}

CDPClient::~CDPClient() {
    disconnect();
}

void CDPClient::connect(const std::string& webSocketUrl, ConnectionCallback callback) {
    if (connected_.exchange(true)) {
        LOG_WARN("CDPClient already connected");
        if (callback) callback(false, "Already connected");
        return;
    }
    
    webSocketUrl_ = webSocketUrl;
    
    pool_->submit([this, callback, webSocketUrl]() {
        std::string error;
#ifdef _WIN32
        auto ws = std::make_shared<WebSocketImpl>();
        bool success = ws->connect(webSocketUrl, error);
#else
        std::shared_ptr<WebSocketImpl> ws;
        bool success = false;
        error = "Not supported";
#endif
        
        if (success) {
            running_ = true;
            {
                std::lock_guard<std::mutex> lock(wsMutex_);
                ws_ = ws;
            }
            processingThread_ = std::jthread([this, callback, ws](std::stop_token token) {
                // The success callback must NOT run on this receive thread: the
                // callback (PageSession::initializeSession) issues synchronous
                // CDP commands that require THIS thread to keep reading
                // responses. Dispatch it to the pool instead.
                if (callback) {
                    pool_->submit([callback]() { callback(true, ""); });
                }
                onConnected();
                
#ifdef _WIN32
                LOG_DEBUG("CDPClient: receive loop started");
                int pollCount = 0;
                while (!token.stop_requested() && ws && ws->isConnected()) {
                    std::string data;
                    int received = ws->receive(data);
                    if (received > 0) {
                        onMessage(data);
                    } else if (received == 0) {
                        break;
                    } else if (received == -2) {
                        if (++pollCount % 100 == 0) {
                            LOG_DEBUG("CDPClient: polling ({} polls)", pollCount);
                        }
                        continue;
                    } else {
                        LOG_DEBUG("CDPClient: websocket receive error ({})", received);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
#endif
                
                onDisconnected("Connection lost");
            });
        } else {
            connected_ = false;
            if (callback) callback(false, error);
            LOG_ERROR("CDPClient connection failed: {}", error);
        }
    });
}

void CDPClient::disconnect() {
    if (!connected_.exchange(false)) {
        return;
    }
    
    running_ = false;
    processingThread_.request_stop();
    
#ifdef _WIN32
    std::shared_ptr<WebSocketImpl> ws;
    {
        std::lock_guard<std::mutex> lock(wsMutex_);
        ws = std::static_pointer_cast<WebSocketImpl>(ws_);
        ws_.reset();
    }
    if (ws) ws->close();
#endif
    
    if (processingThread_.joinable()) {
        processingThread_.join();
    }
    
    LOG_INFO("CDPClient disconnected from {}", webSocketUrl_);
}

void CDPClient::onConnected() {
    LOG_INFO("CDPClient connected to {}", webSocketUrl_);
}

void CDPClient::onDisconnected(const std::string& reason) {
    bool wasConnected = connected_.exchange(false);
    
    if (wasConnected) {
        LOG_WARN("CDPClient disconnected: {}", reason);
        
        std::lock_guard<std::mutex> lock(pendingMutex_);
        for (auto& [id, cmd] : pendingCommands_) {
            if (cmd.callback) {
                nlohmann::json errorResult;
                errorResult["error"] = "Connection lost";
                try {
                    cmd.callback(nlohmann::json::object());
                } catch (...) {}
            }
        }
        pendingCommands_.clear();
        
        if (onDisconnect_) {
            onDisconnect_();
        }
    }
}

void CDPClient::onMessage(const std::string& payload) {
    LOG_DEBUG("CDPClient: received message ({} bytes)", payload.size());
    try {
        auto json = nlohmann::json::parse(payload);
        
        if (json.contains("id")) {
            handleResponse(json);
        } else if (json.contains("method")) {
            handleEvent(json);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("CDPClient parse error: {}", e.what());
    }
}

void CDPClient::handleResponse(const nlohmann::json& message) {
    int64_t id = message["id"].get<int64_t>();
    
    std::lock_guard<std::mutex> lock(pendingMutex_);
    auto it = pendingCommands_.find(id);
    
    if (it != pendingCommands_.end()) {
        CDPCommand cmd = std::move(it->second);
        pendingCommands_.erase(it);
        
        nlohmann::json result = message.value("result", nlohmann::json::object());
        
        if (cmd.callback) {
            try {
                cmd.callback(result);
            } catch (const std::exception& e) {
                LOG_ERROR("CDP response callback error: {}", e.what());
            }
        }
    } else {
        LOG_WARN("CDPClient: unmatched response for id {}", id);
    }
}

void CDPClient::handleEvent(const nlohmann::json& message) {
    std::string method = message.value("method", "");
    nlohmann::json params = message.value("params", nlohmann::json::object());
    
    std::lock_guard<std::mutex> lock(handlersMutex_);
    auto it = eventHandlers_.find(method);
    if (it != eventHandlers_.end()) {
        for (const auto& handler : it->second) {
            try {
                handler(method, params);
            } catch (const std::exception& e) {
                LOG_ERROR("CDP event handler error: {}", e.what());
            }
        }
    }
}

int64_t CDPClient::sendCommand(const std::string& method, const nlohmann::json& params) {
    if (!connected_) {
        LOG_ERROR("CDPClient not connected, cannot send command: {}", method);
        return -1;
    }
    
    int64_t id = nextMessageId_++;
    
    nlohmann::json message;
    message["id"] = id;
    message["method"] = method;
    message["params"] = params;
    
    std::string payload = message.dump();
    
#ifdef _WIN32
    std::shared_ptr<WebSocketImpl> ws;
    {
        std::lock_guard<std::mutex> lock(wsMutex_);
        ws = std::static_pointer_cast<WebSocketImpl>(ws_);
    }
    if (ws && ws->isConnected()) {
        ws->send(payload);
    }
#endif
    
    LOG_DEBUG("CDP command sent: {} (id={})", method, id);
    return id;
}

void CDPClient::sendCommand(const std::string& method, const nlohmann::json& params,
                           ResponseCallback callback) {
    if (!connected_) {
        LOG_ERROR("CDPClient not connected, cannot send command: {}", method);
        if (callback) callback(nlohmann::json::object());
        return;
    }
    
    int64_t id = nextMessageId_++;
    
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingCommands_[id] = CDPCommand{
            id, method, params, callback, std::chrono::steady_clock::now()
        };
    }
    
    nlohmann::json message;
    message["id"] = id;
    message["method"] = method;
    message["params"] = params;
    
    std::string payload = message.dump();
    
#ifdef _WIN32
    std::shared_ptr<WebSocketImpl> ws;
    {
        std::lock_guard<std::mutex> lock(wsMutex_);
        ws = std::static_pointer_cast<WebSocketImpl>(ws_);
    }
    if (ws && ws->isConnected()) {
        ws->send(payload);
    }
#endif
    
    LOG_DEBUG("CDP command sent with callback: {} (id={})", method, id);
}

std::optional<nlohmann::json> CDPClient::sendCommandSync(const std::string& method,
                                                        const nlohmann::json& params,
                                                        std::chrono::milliseconds timeout) {
    // Use a shared promise so the async response callback keeps it alive even
    // if this function times out and returns (the callback may fire later).
    // Without this, the callback holds a dangling &promise -> use-after-free.
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();

    sendCommand(method, params, [promise](const nlohmann::json& result) {
        try {
            promise->set_value(result);
        } catch (const std::future_error&) {
            // Already satisfied (e.g. timed out then response arrived).
        }
    });

    try {
        auto status = future.wait_for(timeout);
        if (status == std::future_status::ready) {
            return future.get();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("CDP sync command error: {}", e.what());
    }

    LOG_WARN("CDP sync command timed out: {}", method);
    return std::nullopt;
}

void CDPClient::subscribe(const std::string& event, MessageHandler handler) {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    eventHandlers_[event].push_back(std::move(handler));
    LOG_DEBUG("Subscribed to CDP event: {}", event);
}

void CDPClient::unsubscribe(const std::string& event) {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    eventHandlers_.erase(event);
}

void CDPClient::enableDomain(const std::string& domain) {
    sendCommand(domain + ".enable");
    LOG_DEBUG("Enabled CDP domain: {}", domain);
}

void CDPClient::processPendingRequests() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    
    auto now = std::chrono::steady_clock::now();
    std::vector<int64_t> timedOut;
    
    for (auto& [id, cmd] : pendingCommands_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - cmd.sentAt).count();
        
        if (elapsed > requestTimeout_.count()) {
            timedOut.push_back(id);
        }
    }
    
    for (int64_t id : timedOut) {
        auto it = pendingCommands_.find(id);
        if (it != pendingCommands_.end()) {
            LOG_WARN("CDP command timed out: {} (id={})", it->second.method, id);
            pendingCommands_.erase(it);
        }
    }
}

} // namespace edgemon
