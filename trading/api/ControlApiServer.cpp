#include "trading/api/ControlApiServer.hpp"
#include "logger.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

namespace trading {

namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpBad = 400;
constexpr int kHttpUnauthorized = 401;
constexpr int kHttpNotFound = 404;

std::string b64Encode(const unsigned char* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string out(bptr->data, bptr->length);
    BIO_free_all(b64);
    return out;
}

std::string sha1Base64(const std::string& in) {
    unsigned char digest[20];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, in.data(), in.size());
    EVP_DigestFinal_ex(ctx, digest, nullptr);
    EVP_MD_CTX_free(ctx);
    return b64Encode(digest, 20);
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string urlDecodePath(const std::string& p) {
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '%' && i + 2 < p.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(p[i + 1]), l = hex(p[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 2;
                continue;
            }
        }
        out.push_back(p[i]);
    }
    return out;
}

bool sendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(s, data + sent, static_cast<int>(len - sent), 0);
        if (n == SOCKET_ERROR) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvLine(SOCKET s, std::string& line) {
    line.clear();
    char c;
    while (true) {
        int n = ::recv(s, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
    }
}

// Sends a minimal HTTP response.
void sendResponse(SOCKET s, int status, const std::string& contentType,
                  const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " "
        << (status == 200 ? "OK" : status == 400 ? "Bad Request"
                               : status == 401   ? "Unauthorized"
                                                 : "Not Found")
        << "\r\nContent-Type: " << contentType
        << "\r\nContent-Length: " << body.size()
        << "\r\nConnection: close\r\n\r\n";
    std::string head = oss.str();
    sendAll(s, head.data(), head.size());
    sendAll(s, body.data(), body.size());
}

} // namespace

ControlApiServer::ControlApiServer(IControlBackend& backend,
                                   const ApiConfig& cfg)
    : backend_(backend), cfg_(cfg) {}

ControlApiServer::~ControlApiServer() {
    stop();
}

bool ControlApiServer::start() {
    if (running_.exchange(true)) return false;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void ControlApiServer::stop() {
    if (!running_.exchange(false)) return;
    // Closing the accept socket is handled inside acceptLoop via a control
    // flag; simplest robust approach: join with a short timeout by closing
    // all tracked ws clients and relying on the accept loop's bounded wait.
    if (acceptThread_.joinable()) acceptThread_.join();
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (uintptr_t c : wsClients_) {
        shutdown(static_cast<SOCKET>(c), SD_BOTH);
        closesocket(static_cast<SOCKET>(c));
    }
    wsClients_.clear();
}

void ControlApiServer::broadcast(const BusEvent& event) {
    nlohmann::json j;
    j["type"] = toString(event.type);
    j["ts"] = event.timestampNs;
    j["seq"] = event.sequence;
    j["source"] = event.source;
    j["symbol"] = event.symbol;
    if (!event.payload.empty()) {
        try {
            j["data"] = nlohmann::json::parse(event.payload);
        } catch (...) {
            j["data"] = event.payload;
        }
    }
    std::string frame = j.dump();
    // Wrap in a WS text frame.
    std::string out;
    out.push_back(static_cast<char>(0x81));
    size_t len = frame.size();
    if (len < 126) {
        out.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(127);
        uint64_t l = len;
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<char>((l >> (i * 8)) & 0xFF));
    }
    out += frame;

    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (uintptr_t c : wsClients_) {
        sendAll(static_cast<SOCKET>(c), out.data(), out.size());
    }
}

void ControlApiServer::acceptLoop() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("ControlApiServer: WSAStartup failed");
        running_.store(false);
        return;
    }
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        LOG_ERROR("ControlApiServer: socket failed");
        running_.store(false);
        return;
    }
    // Bind only to the configured loopback host.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    if (cfg_.bindHost == "127.0.0.1" || cfg_.bindHost.empty() ||
        cfg_.bindHost == "localhost") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (cfg_.bindHost == "::1") {
        // IPv6 loopback not supported by this minimal server; fall back to v4.
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        LOG_ERROR("ControlApiServer: refusing to bind non-loopback host {}",
                  cfg_.bindHost);
        closesocket(listenSock);
        running_.store(false);
        return;
    }
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("ControlApiServer: bind 127.0.0.1:{} failed err={}", cfg_.port,
                  WSAGetLastError());
        closesocket(listenSock);
        running_.store(false);
        return;
    }
    if (listen(listenSock, 8) != 0) {
        LOG_ERROR("ControlApiServer: listen failed");
        closesocket(listenSock);
        running_.store(false);
        return;
    }
    LOG_INFO("ControlApiServer: listening on {}:{}", cfg_.bindHost, cfg_.port);

    // Accept loop with a poll so stop() can interrupt.
    u_long nb = 1;
    ioctlsocket(listenSock, FIONBIO, &nb);
    while (running_.load()) {
        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (!running_.load()) break;
            continue;
        }
        // Handle each client on its own detached thread (short-lived HTTP, or
        // a persistent WS connection).
        u_long blk = 0;
        ioctlsocket(client, FIONBIO, &blk);
        std::thread([this, client]() { handleClient(static_cast<uintptr_t>(client)); })
            .detach();
    }
    closesocket(listenSock);
}

void ControlApiServer::handleClient(uintptr_t clientSockPtr) {
    SOCKET client = static_cast<SOCKET>(clientSockPtr);
    // Read request line + headers.
    std::string requestLine;
    if (!recvLine(client, requestLine)) {
        closesocket(client);
        return;
    }
    std::istringstream iss(requestLine);
    std::string method, path, version;
    iss >> method >> path >> version;

    std::map<std::string, std::string> headers;
    std::string line;
    int contentLength = 0;
    while (recvLine(client, line)) {
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = lower(line.substr(0, colon));
            std::string v = trim(line.substr(colon + 1));
            headers[k] = v;
            if (k == "content-length") contentLength = std::atoi(v.c_str());
        }
    }
    std::string body;
    if (contentLength > 0 && contentLength < 1'000'000) {
        body.resize(contentLength);
        size_t got = 0;
        while (got < static_cast<size_t>(contentLength)) {
            int n = ::recv(client, &body[got],
                           static_cast<int>(contentLength - got), 0);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        body.resize(got);
    }

    std::string respBody, contentType = "application/json";
    bool wsUpgrade = false;
    std::string wsKey;
    handleRequest(method, urlDecodePath(path), headers, body, respBody,
                  contentType, wsUpgrade, wsKey);

    if (wsUpgrade) {
        // Perform the WebSocket handshake and then stream events.
        std::string acceptKey = sha1Base64(wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        std::ostringstream oss;
        oss << "HTTP/1.1 101 Switching Protocols\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Accept: " << acceptKey << "\r\n\r\n";
        std::string hs = oss.str();
        if (!sendAll(client, hs.data(), hs.size())) {
            closesocket(client);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            wsClients_.push_back(static_cast<uintptr_t>(client));
        }
        // Keep the connection open; drop when the client disconnects.
        // The broadcast() path pushes events; here we just idle-read to detect
        // closure, then remove the client.
        char buf[256];
        while (running_.load()) {
            int n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
        }
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            auto it = std::find(wsClients_.begin(), wsClients_.end(),
                                static_cast<uintptr_t>(client));
            if (it != wsClients_.end()) wsClients_.erase(it);
        }
        closesocket(client);
        return;
    }
    sendResponse(client, respBody.empty() ? kHttpNotFound : kHttpOk,
                 contentType, respBody);
    closesocket(client);
}

void ControlApiServer::handleRequest(
    const std::string& method, const std::string& path,
    const std::map<std::string, std::string>& headers,
    const std::string& body, std::string& response, std::string& contentType,
    bool& wsUpgrade, std::string& wsAcceptKey) {
    // Auth check (bearer token).
    if (cfg_.requireAuth && !cfg_.bearerToken.empty()) {
        auto it = headers.find("authorization");
        std::string expected = "Bearer " + cfg_.bearerToken;
        if (it == headers.end() || it->second != expected) {
            response = nlohmann::json{{"error", "unauthorized"}}.dump();
            return;
        }
    }

    // WebSocket event stream (spec §43).
    if (path == "/ws" || path == "/events") {
        auto upIt = headers.find("upgrade");
        auto keyIt = headers.find("sec-websocket-key");
        if (upIt != headers.end() &&
            lower(upIt->second).find("websocket") != std::string::npos &&
            keyIt != headers.end()) {
            wsUpgrade = true;
            wsAcceptKey = keyIt->second;
            return;
        }
        response = nlohmann::json{{"error", "websocket upgrade required"}}.dump();
        return;
    }

    // JSON body helper for POST.
    auto parseBody = [&]() -> nlohmann::json {
        if (body.empty()) return nlohmann::json::object();
        try {
            return nlohmann::json::parse(body);
        } catch (...) {
            return nlohmann::json::object();
        }
    };

    if (method == "GET" && path == "/health") {
        response = nlohmann::json{{"status", "ok"}}.dump();
    } else if (method == "GET" && path == "/status") {
        response = backend_.status().dump();
    } else if (method == "GET" && path == "/connectors") {
        response = backend_.connectors().dump();
    } else if (method == "POST" && path == "/connect") {
        auto j = parseBody();
        response = backend_.connect(j.value("type", ""), j.value("env", "PAPER"),
                                    j.value("options", nlohmann::json::object()))
                       .dump();
    } else if (method == "POST" && path == "/disconnect") {
        auto j = parseBody();
        response = backend_.disconnect(j.value("name", "")).dump();
    } else if (method == "GET" && path == "/positions") {
        response = backend_.positions().dump();
    } else if (method == "GET" && path == "/orders") {
        response = backend_.orders().dump();
    } else if (method == "POST" && path == "/orders") {
        response = backend_.placeOrder(parseBody()).dump();
    } else if (method == "POST" && path.rfind("/orders/cancel", 0) == 0) {
        auto j = parseBody();
        response = backend_.cancelOrder(j.value("order_id", "")).dump();
    } else if (method == "GET" && path == "/signals") {
        response = backend_.signals().dump();
    } else if (method == "GET" && path == "/risk") {
        response = backend_.risk().dump();
    } else if (method == "POST" && path == "/kill-switch") {
        auto j = parseBody();
        response = backend_.killSwitch(j.value("engage", true)).dump();
    } else {
        response = "";
    }
}

} // namespace trading
