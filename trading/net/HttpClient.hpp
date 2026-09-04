#pragma once

// trading/net/HttpClient.hpp — minimal blocking HTTP/1.1 client over
// OpenSSL TLS (or plain TCP). Supports GET/POST/DELETE with headers and body.
// Certificate verification is ON by default (spec §36 — never disable TLS
// verification). Used for broker REST APIs and the local AI service.

#include <string>
#include <map>
#include <optional>
#include <chrono>
#include <functional>

namespace trading {

struct HttpRequest {
    std::string method = "GET";
    std::string url;                       // https://host[:port]/path
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{15000};
    bool verifyTls = true;                 // ALWAYS keep true in production
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
    bool transportError = false;           // DNS/TLS/connect/read failure
    std::string error;                     // human readable
};

class HttpClient {
public:
    HttpClient() = default;
    ~HttpClient() = default;

    HttpResponse request(const HttpRequest& req);

    // Convenience wrappers.
    HttpResponse get(const std::string& url,
                     const std::map<std::string, std::string>& headers = {},
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(15000));
    HttpResponse post(const std::string& url, const std::string& body,
                      const std::map<std::string, std::string>& headers = {},
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(15000));
    HttpResponse del(const std::string& url,
                     const std::map<std::string, std::string>& headers = {},
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(15000));

    // Global one-time init for sockets/OpenSSL. Safe to call repeatedly.
    static bool ensureInit();
};

} // namespace trading
