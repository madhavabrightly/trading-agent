#include "trading/net/HttpClient.hpp"
#include "trading/net/TlsInit.hpp"
#include "logger.hpp"

#include <cstring>
#include <sstream>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace trading {

namespace {

// --- socket + TLS plumbing -------------------------------------------------

struct SocketCloser {
    SOCKET s = INVALID_SOCKET;
    ~SocketCloser() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
};

struct SslCleanup {
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    ~SslCleanup() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
    }
};

bool resolve(const std::string& host, int port, sockaddr_in& out,
             std::string& error) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0) {
        error = "DNS resolution failed for " + host + ": " + gai_strerror(rc);
        return false;
    }
    if (!res) {
        error = "no addresses for " + host;
        return false;
    }
    std::memcpy(&out, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);
    return true;
}

bool connectSocket(SOCKET sock, const sockaddr_in& addr, int timeoutMs,
                   std::string& error) {
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
    int rc = ::connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(sock, &wset);
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            int sel = select(0, nullptr, &wset, nullptr, &tv);
            if (sel <= 0 || !FD_ISSET(sock, &wset)) {
                error = "connect timeout";
                return false;
            }
            int soErr = 0;
            socklen_t soLen = sizeof(soErr);
            getsockopt(sock, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&soErr), &soLen);
            if (soErr != 0) {
                error = "connect failed: " + std::to_string(soErr);
                return false;
            }
        } else {
            error = "connect failed: " + std::to_string(err);
            return false;
        }
    }
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    return true;
}

bool waitWritable(SOCKET sock, int timeoutMs) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(sock, &wset);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(0, nullptr, &wset, nullptr, &tv) > 0;
}

bool waitReadable(SOCKET sock, int timeoutMs) {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(0, &rset, nullptr, nullptr, &tv) > 0;
}

bool sendAll(SOCKET sock, const char* data, size_t len, int timeoutMs) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(sock, data + sent, static_cast<int>(len - sent), 0);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                if (!waitWritable(sock, timeoutMs)) return false;
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool sendTlsAll(SSL* ssl, const char* data, size_t len, int timeoutMs) {
    size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(ssl, data + sent, static_cast<int>(len - sent));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                // Spin with a bounded wait on the socket.
                int fd = SSL_get_fd(ssl);
                if (!waitWritable(static_cast<SOCKET>(fd), timeoutMs)) return false;
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Reads one CRLF-terminated line from either transport. Returns false on EOF
// or timeout/error.
template <typename ReadOne>
bool readLine(ReadOne&& readOne, std::string& out) {
    out.clear();
    char c;
    while (true) {
        int rc = readOne(&c, 1);
        if (rc <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') out.push_back(c);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// HttpClient
// ---------------------------------------------------------------------------

bool HttpClient::ensureInit() {
    static bool inited = false;
    if (inited) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    inited = true;
    return true;
}

HttpResponse HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    std::chrono::milliseconds timeout) {
    HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.headers = headers;
    req.timeout = timeout;
    return request(req);
}

HttpResponse HttpClient::post(
    const std::string& url, const std::string& body,
    const std::map<std::string, std::string>& headers,
    std::chrono::milliseconds timeout) {
    HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.body = body;
    req.headers = headers;
    req.timeout = timeout;
    return request(req);
}

HttpResponse HttpClient::del(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    std::chrono::milliseconds timeout) {
    HttpRequest req;
    req.method = "DELETE";
    req.url = url;
    req.headers = headers;
    req.timeout = timeout;
    return request(req);
}

// Reads the full HTTP response body honoring Content-Length / chunked /
// close-delimited framing over a ReadN callable.
template <typename ReadN>
void readBody(ReadN&& readN, const std::map<std::string, std::string>& headers,
              HttpResponse& resp, bool& transportError, std::string& error) {
    auto it = headers.find("content-length");
    auto teIt = headers.find("transfer-encoding");
    bool chunked = teIt != headers.end() &&
                   teIt->second.find("chunked") != std::string::npos;
    if (chunked) {
        std::string sizeLine;
        while (true) {
            if (!readLine(readN, sizeLine)) { transportError = true; return; }
            size_t chunkSize = std::strtoull(sizeLine.c_str(), nullptr, 16);
            if (chunkSize == 0) {
                // Trailing CRLF after the terminal chunk.
                std::string trailer;
                if (!readLine(readN, trailer)) { transportError = true; return; }
                break;
            }
            size_t got = 0;
            while (got < chunkSize) {
                char buf[2048];
                size_t want = std::min(sizeof(buf), chunkSize - got);
                int n = readN(buf, static_cast<int>(want));
                if (n <= 0) { transportError = true; return; }
                resp.body.append(buf, static_cast<size_t>(n));
                got += static_cast<size_t>(n);
            }
            // CRLF after chunk data.
            std::string crlf;
            if (!readLine(readN, crlf)) { transportError = true; return; }
        }
        return;
    }
    if (it != headers.end()) {
        size_t contentLength = std::strtoull(it->second.c_str(), nullptr, 10);
        resp.body.reserve(contentLength);
        while (resp.body.size() < contentLength) {
            char buf[4096];
            size_t want = std::min(sizeof(buf), contentLength - resp.body.size());
            int n = readN(buf, static_cast<int>(want));
            if (n <= 0) { transportError = true; return; }
            resp.body.append(buf, static_cast<size_t>(n));
        }
        return;
    }
    // Close-delimited: read until EOF.
    char buf[4096];
    while (true) {
        int n = readN(buf, sizeof(buf));
        if (n <= 0) break;
        resp.body.append(buf, static_cast<size_t>(n));
    }
}

HttpResponse HttpClient::request(const HttpRequest& req) {
    HttpResponse resp;
    if (!ensureInit()) {
        resp.transportError = true;
        resp.error = "socket init failed";
        return resp;
    }

    // --- Parse URL ---
    size_t schemeEnd = req.url.find("://");
    if (schemeEnd == std::string::npos) {
        resp.transportError = true;
        resp.error = "invalid url (no scheme)";
        return resp;
    }
    std::string scheme = req.url.substr(0, schemeEnd);
    bool tls = scheme == "https";
    if (scheme != "http" && scheme != "https") {
        resp.transportError = true;
        resp.error = "unsupported scheme: " + scheme;
        return resp;
    }
    size_t hostStart = schemeEnd + 3;
    size_t hostEnd = req.url.find('/', hostStart);
    if (hostEnd == std::string::npos) hostEnd = req.url.size();
    std::string hostPort = req.url.substr(hostStart, hostEnd - hostStart);
    size_t portColon = hostPort.find(':');
    std::string host = portColon != std::string::npos
                           ? hostPort.substr(0, portColon)
                           : hostPort;
    std::string path = hostEnd < req.url.size() ? req.url.substr(hostEnd) : "/";
    if (path.empty()) path = "/";
    int port = tls ? 443 : 80;
    if (portColon != std::string::npos) {
        try {
            port = std::stoi(hostPort.substr(portColon + 1));
        } catch (...) {
            resp.transportError = true;
            resp.error = "invalid port";
            return resp;
        }
    }

    // --- Connect ---
    sockaddr_in addr{};
    std::string error;
    if (!resolve(host, port, addr, error)) {
        resp.transportError = true;
        resp.error = error;
        return resp;
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        resp.transportError = true;
        resp.error = "socket() failed";
        return resp;
    }
    SocketCloser closer{sock};
    int timeoutMs = static_cast<int>(req.timeout.count());
    if (!connectSocket(sock, addr, timeoutMs, error)) {
        resp.transportError = true;
        resp.error = error;
        return resp;
    }

    // --- Build request ---
    std::string head;
    head.reserve(512);
    head += req.method + " " + path + " HTTP/1.1\r\n";
    head += "Host: " + host;
    if ((tls && port != 443) || (!tls && port != 80))
        head += ":" + std::to_string(port);
    head += "\r\nUser-Agent: trading-core/0.1\r\nAccept: */*\r\n";
    if (!req.body.empty()) {
        head += "Content-Type: application/json\r\n";
        head += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    }
    for (const auto& [k, v] : req.headers) head += k + ": " + v + "\r\n";
    head += "Connection: close\r\n\r\n";

    // --- Transport send + response read ---
    auto parseResponse = [&](auto&& readOne, auto&& readN) -> bool {
        // Status line
        std::string statusLine;
        if (!readLine(readOne, statusLine)) {
            resp.transportError = true;
            resp.error = "read failed (status line)";
            return false;
        }
        std::istringstream iss(statusLine);
        std::string httpVer;
        iss >> httpVer >> resp.status;
        if (resp.status == 0) {
            resp.transportError = true;
            resp.error = "malformed status line";
            return false;
        }
        // Headers
        while (true) {
            std::string line;
            if (!readLine(readOne, line)) {
                resp.transportError = true;
                resp.error = "read failed (headers)";
                return false;
            }
            if (line.empty()) break;
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                if (!v.empty() && v.front() == ' ') v.erase(v.begin());
                std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                resp.headers[k] = v;
            }
        }
        readBody(readN, resp.headers, resp, resp.transportError, resp.error);
        return !resp.transportError;
    };

    if (!tls) {
        if (!sendAll(sock, head.data(), head.size(), timeoutMs) ||
            (!req.body.empty() &&
             !sendAll(sock, req.body.data(), req.body.size(), timeoutMs))) {
            resp.transportError = true;
            resp.error = "send failed";
            return resp;
        }
        auto readOne = [&](char* c, int n) -> int {
            return ::recv(sock, c, n, 0);
        };
        auto readN = [&](char* buf, int n) -> int {
            size_t got = 0;
            while (got < static_cast<size_t>(n)) {
                int r = ::recv(sock, buf + got, static_cast<int>(n - got), 0);
                if (r == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) {
                        if (!waitReadable(sock, timeoutMs)) return -1;
                        continue;
                    }
                    return -1;
                }
                if (r == 0) break;
                got += static_cast<size_t>(r);
                // If the caller asked for one byte, return it now.
                if (n == 1) break;
            }
            return static_cast<int>(got);
        };
        // recv is blocking (we cleared non-blocking after connect).
        parseResponse(readOne, readN);
    } else {
        SslCleanup sc;
        sc.ctx = createVerifiedClientCtx(error);
        if (!sc.ctx) {
            resp.transportError = true;
            resp.error = error;
            return resp;
        }
        sc.ssl = SSL_new(sc.ctx);
        if (!sc.ssl) {
            resp.transportError = true;
            resp.error = "SSL_new failed";
            return resp;
        }
        SSL_set_fd(sc.ssl, static_cast<int>(sock));
        SSL_set_tlsext_host_name(sc.ssl, host.c_str());
        if (req.verifyTls) {
            X509_VERIFY_PARAM* param = SSL_get0_param(sc.ssl);
            X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0);
        } else {
            SSL_CTX_set_verify(sc.ctx, SSL_VERIFY_NONE, nullptr);
            LOG_WARN("HttpClient: TLS verification DISABLED for {}", req.url);
        }
        if (SSL_connect(sc.ssl) != 1) {
            unsigned long e = ERR_get_error();
            char buf[256];
            ERR_error_string_n(e, buf, sizeof(buf));
            resp.transportError = true;
            resp.error = "TLS handshake failed: " + std::string(buf);
            return resp;
        }
        if (!sendTlsAll(sc.ssl, head.data(), head.size(), timeoutMs) ||
            (!req.body.empty() &&
             !sendTlsAll(sc.ssl, req.body.data(), req.body.size(), timeoutMs))) {
            resp.transportError = true;
            resp.error = "TLS send failed";
            return resp;
        }
        auto readOne = [&](char* c, int n) -> int {
            int r;
            do {
                r = SSL_read(sc.ssl, c, n);
                if (r <= 0) {
                    int err = SSL_get_error(sc.ssl, r);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        if (!waitReadable(sock, timeoutMs)) return -1;
                        continue;
                    }
                    return -1;
                }
            } while (false);
            return r;
        };
        auto readN = [&](char* buf, int n) -> int {
            int r;
            do {
                r = SSL_read(sc.ssl, buf, n);
                if (r <= 0) {
                    int err = SSL_get_error(sc.ssl, r);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        if (!waitReadable(sock, timeoutMs)) return -1;
                        continue;
                    }
                    return -1;
                }
            } while (false);
            return r;
        };
        parseResponse(readOne, readN);
    }
    return resp;
}

} // namespace trading
