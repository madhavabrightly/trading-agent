#include "trading/net/WebSocketClient.hpp"
#include "trading/net/TlsInit.hpp"
#include "logger.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/buffer.h>

#include <cstring>
#include <sstream>
#include <thread>

namespace trading {

namespace {

constexpr int kOpText = 0x1;
constexpr int kOpBinary = 0x2;
constexpr int kOpClose = 0x8;
constexpr int kOpPing = 0x9;
constexpr int kOpPong = 0xA;

bool waitSocket(SOCKET s, bool write, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = select(0, write ? nullptr : &fds, write ? &fds : nullptr, nullptr, &tv);
    return rc > 0;
}

std::string base64Encode(const unsigned char* data, size_t len) {
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

} // namespace

struct WebSocketClient::Impl {
    SOCKET sock = INVALID_SOCKET;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    bool tls = false;
    std::string host;
    std::string path;
    int port = 80;
    int timeoutMs = 10000;
    std::thread reader;
    std::atomic<bool> stop{false};

    ~Impl() {
        stop.store(true);
        if (reader.joinable()) reader.join();
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        if (sock != INVALID_SOCKET) closesocket(sock);
    }

    // --- transport-level I/O (raw or TLS) ---
    bool sendAll(const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            int n;
            if (!tls) {
                n = ::send(sock, data + sent, static_cast<int>(len - sent), 0);
                if (n == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) {
                        if (!waitSocket(sock, true, timeoutMs)) return false;
                        continue;
                    }
                    return false;
                }
            } else {
                n = SSL_write(ssl, data + sent, static_cast<int>(len - sent));
                if (n <= 0) {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                        if (!waitSocket(sock, true, timeoutMs)) return false;
                        continue;
                    }
                    return false;
                }
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Reads exactly len bytes. Returns false on EOF/timeout/error.
    bool readExact(char* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            int n;
            if (!tls) {
                n = ::recv(sock, buf + got, static_cast<int>(len - got), 0);
                if (n == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) {
                        if (!waitSocket(sock, false, timeoutMs)) return false;
                        continue;
                    }
                    return false;
                }
                if (n == 0) return false;
            } else {
                n = SSL_read(ssl, buf + got, static_cast<int>(len - got));
                if (n <= 0) {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        if (!waitSocket(sock, false, timeoutMs)) return false;
                        continue;
                    }
                    return false;
                }
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }

    // Reads one line (CRLF or LF terminated, CR stripped). false on EOF/timeout.
    bool readLine(std::string& out) {
        out.clear();
        char c;
        while (true) {
            if (!readExact(&c, 1)) return false;
            if (c == '\n') return true;
            if (c != '\r') out.push_back(c);
        }
    }

    // Reads a single WS frame. Returns true and fills opcode+payload.
    bool readFrame(int& opcode, std::string& payload, uint64_t maxFrameBytes) {
        unsigned char hdr[2];
        if (!readExact(reinterpret_cast<char*>(hdr), 2)) return false;
        opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7F;
        if (len == 126) {
            unsigned char ext[2];
            if (!readExact(reinterpret_cast<char*>(ext), 2)) return false;
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8];
            if (!readExact(reinterpret_cast<char*>(ext), 8)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
        }
        if (len > maxFrameBytes) return false;  // sanity cap
        unsigned char maskKey[4] = {0, 0, 0, 0};
        if (masked) {
            if (!readExact(reinterpret_cast<char*>(maskKey), 4)) return false;
        }
        payload.resize(static_cast<size_t>(len));
        if (len > 0) {
            if (!readExact(&payload[0], static_cast<size_t>(len))) return false;
            if (masked) {
                for (uint64_t i = 0; i < len; ++i)
                    payload[static_cast<size_t>(i)] ^= maskKey[i % 4];
            }
        }
        return true;
    }

    bool sendFrame(int opcode, const std::string& payload) {
        std::string frame;
        frame.reserve(payload.size() + 10);
        frame.push_back(static_cast<char>(0x80 | opcode));
        size_t len = payload.size();
        if (len < 126) {
            frame.push_back(static_cast<char>(len));
        } else if (len <= 0xFFFF) {
            frame.push_back(126);
            frame.push_back(static_cast<char>((len >> 8) & 0xFF));
            frame.push_back(static_cast<char>(len & 0xFF));
        } else {
            frame.push_back(127);
            uint64_t l = len;
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<char>((l >> (i * 8)) & 0xFF));
        }
        frame.append(payload);
        return sendAll(frame.data(), frame.size());
    }
};

WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) {}

WebSocketClient::~WebSocketClient() {
    close();
}

bool WebSocketClient::connect(const std::string& url, int timeoutMs,
                              std::string* errorOut) {
    // Parse URL.
    impl_->tls = url.rfind("wss://", 0) == 0;
    if (!impl_->tls && url.rfind("ws://", 0) != 0) {
        if (errorOut) *errorOut = "unsupported websocket url";
        return false;
    }
    std::string rest = impl_->tls ? url.substr(6) : url.substr(5);
    auto slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    impl_->path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = hostPort.find(':');
    if (colon != std::string::npos) {
        impl_->host = hostPort.substr(0, colon);
        impl_->port = std::stoi(hostPort.substr(colon + 1));
    } else {
        impl_->host = hostPort;
        impl_->port = impl_->tls ? 443 : 80;
    }
    impl_->timeoutMs = timeoutMs;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (errorOut) *errorOut = "WSAStartup failed";
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        if (errorOut) *errorOut = "socket failed";
        return false;
    }
    impl_->sock = sock;

    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);

    hostent* he = gethostbyname(impl_->host.c_str());
    if (!he) {
        if (errorOut) *errorOut = "DNS failed: " + impl_->host;
        impl_->sock = INVALID_SOCKET;
        closesocket(sock);
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(impl_->port));
    std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int rc = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            if (errorOut) *errorOut = "connect failed: " + std::to_string(err);
            closesocket(sock);
            impl_->sock = INVALID_SOCKET;
            return false;
        }
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (select(0, nullptr, &wset, nullptr, &tv) <= 0 || !FD_ISSET(sock, &wset)) {
            if (errorOut) *errorOut = "connect timeout";
            closesocket(sock);
            impl_->sock = INVALID_SOCKET;
            return false;
        }
    }
    u_long blk = 0;
    ioctlsocket(sock, FIONBIO, &blk);

    // TLS handshake.
    if (impl_->tls) {
        std::string tlsErr;
        impl_->ctx = createVerifiedClientCtx(tlsErr);
        if (!impl_->ctx) {
            if (errorOut) *errorOut = tlsErr;
            closesocket(sock);
            impl_->sock = INVALID_SOCKET;
            return false;
        }
        impl_->ssl = SSL_new(impl_->ctx);
        if (!impl_->ssl) {
            if (errorOut) *errorOut = "SSL_new failed";
            closesocket(sock);
            impl_->sock = INVALID_SOCKET;
            return false;
        }
        SSL_set_fd(impl_->ssl, static_cast<int>(sock));
        SSL_set_tlsext_host_name(impl_->ssl, impl_->host.c_str());
        X509_VERIFY_PARAM* param = SSL_get0_param(impl_->ssl);
        X509_VERIFY_PARAM_set1_host(param, impl_->host.c_str(), 0);
        if (SSL_connect(impl_->ssl) != 1) {
            if (errorOut) *errorOut = "TLS handshake failed";
            closesocket(sock);
            impl_->sock = INVALID_SOCKET;
            return false;
        }
    }

    // WebSocket upgrade.
    unsigned char keyBytes[16];
    RAND_bytes(keyBytes, sizeof(keyBytes));
    std::string secKey = base64Encode(keyBytes, sizeof(keyBytes));
    std::ostringstream req;
    req << "GET " << impl_->path << " HTTP/1.1\r\n"
        << "Host: " << impl_->host << ":" << impl_->port << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << secKey << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n"
        << "User-Agent: trading-core/0.1\r\n\r\n";
    std::string reqStr = req.str();
    if (!impl_->sendAll(reqStr.data(), reqStr.size())) {
        if (errorOut) *errorOut = "handshake send failed";
        return false;
    }
    std::string statusLine;
    if (!impl_->readLine(statusLine)) {
        if (errorOut) *errorOut = "no handshake response";
        return false;
    }
    if (statusLine.find(" 101 ") == std::string::npos) {
        if (errorOut) *errorOut = "handshake rejected: " + statusLine;
        return false;
    }
    std::string line;
    while (true) {
        if (!impl_->readLine(line)) {
            if (errorOut) *errorOut = "header read failed";
            return false;
        }
        if (line.empty()) break;
    }

    connected_.store(true);
    impl_->stop.store(false);

    // Reader thread.
    impl_->reader = std::thread([this] {
        int opcode = 0;
        std::string payload;
        while (!impl_->stop.load()) {
            if (!impl_->readFrame(opcode, payload, 16 * 1024 * 1024)) {
                break;
            }
            if (opcode == kOpPing) {
                impl_->sendFrame(kOpPong, payload);
                continue;
            }
            if (opcode == kOpClose) {
                break;
            }
            if (msgHandler_ && (opcode == kOpText || opcode == kOpBinary)) {
                try {
                    msgHandler_(opcode, payload);
                } catch (const std::exception& e) {
                    LOG_ERROR("WebSocketClient: message handler threw: {}", e.what());
                }
            }
        }
        connected_.store(false);
        if (closeHandler_) {
            try {
                closeHandler_("connection closed");
            } catch (...) {}
        }
    });
    return true;
}

bool WebSocketClient::sendText(const std::string& text) {
    if (!connected_.load()) return false;
    return impl_->sendFrame(kOpText, text);
}

bool WebSocketClient::sendPing(const std::string& payload) {
    if (!connected_.load()) return false;
    return impl_->sendFrame(kOpPing, payload);
}

void WebSocketClient::close() {
    impl_->stop.store(true);
    if (impl_->reader.joinable()) impl_->reader.join();
    if (impl_->ssl) {
        SSL_shutdown(impl_->ssl);
    }
    if (impl_->sock != INVALID_SOCKET) {
        closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
    connected_.store(false);
}

} // namespace trading
