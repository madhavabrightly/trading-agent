#include "edge/UrlUtils.hpp"
#include <cctype>
#include <algorithm>

namespace edgemon {

namespace {

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trimCopy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string stripTrailingSlashes(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

} // namespace

std::optional<ParsedUrl> UrlUtils::parse(const std::string& rawUrl) {
    std::string url = trimCopy(rawUrl);
    if (url.empty()) return std::nullopt;

    size_t schemePos = url.find("://");
    if (schemePos == std::string::npos) {
        // Bare host or host/path: assume https (matches CLI behavior).
        if (url.find('.') == std::string::npos &&
            url.rfind("localhost", 0) != 0) {
            return std::nullopt;
        }
        url = "https://" + url;
        schemePos = url.find("://");
    }

    ParsedUrl p;
    p.scheme = toLowerAscii(url.substr(0, schemePos));
    if (p.scheme != "http" && p.scheme != "https") return std::nullopt;

    std::string rest = url.substr(schemePos + 3);

    // Fragment.
    size_t hashPos = rest.find('#');
    if (hashPos != std::string::npos) {
        p.fragment = rest.substr(hashPos + 1);
        rest = rest.substr(0, hashPos);
    }

    // Query.
    size_t queryPos = rest.find('?');
    if (queryPos != std::string::npos) {
        p.query = rest.substr(queryPos + 1);
        rest = rest.substr(0, queryPos);
    }

    // Authority up to first '/'.
    size_t pathPos = rest.find('/');
    std::string authority = pathPos == std::string::npos ? rest : rest.substr(0, pathPos);
    p.path = pathPos == std::string::npos ? "/" : rest.substr(pathPos);

    // Host[:port] — IPv6 brackets preserved for matching simplicity.
    std::string hostPort = authority;
    std::string host = authority;
    int port = -1;
    if (!host.empty() && host.front() == '[') {
        size_t close = host.find(']');
        if (close != std::string::npos) {
            host = host.substr(0, close + 1);
            hostPort = host;
            if (close + 1 < authority.size() && authority[close + 1] == ':') {
                try {
                    port = std::stoi(authority.substr(close + 2));
                } catch (...) { return std::nullopt; }
            }
        } else {
            return std::nullopt;
        }
    } else {
        size_t colon = host.find_last_of(':');
        if (colon != std::string::npos) {
            std::string portStr = host.substr(colon + 1);
            bool allDigits = !portStr.empty() &&
                std::all_of(portStr.begin(), portStr.end(),
                            [](unsigned char c) { return std::isdigit(c); });
            if (allDigits) {
                try {
                    port = std::stoi(portStr);
                } catch (...) { return std::nullopt; }
                host = host.substr(0, colon);
            }
        }
    }

    p.host = toLowerAscii(host);
    bool defaultPort = (p.scheme == "http" && port == 80) ||
                       (p.scheme == "https" && port == 443);
    if (port >= 0 && !defaultPort) {
        p.hostPort = p.host + ":" + std::to_string(port);
    } else {
        p.hostPort = p.host;
    }

    // Root "/" is kept; anything deeper has trailing slashes stripped.
    p.path = stripTrailingSlashes(p.path);
    if (p.path.empty()) p.path = "/";

    return p;
}

std::string UrlUtils::normalize(std::string url) {
    url = trimCopy(url);
    if (url.empty()) return "";
    auto parsed = parse(url);
    if (!parsed) return "";
    std::string out = parsed->scheme + "://" + parsed->hostPort + parsed->path;
    if (!parsed->query.empty()) out += "?" + parsed->query;
    return out;
}

std::string UrlUtils::originAndPath(const std::string& url) {
    auto parsed = parse(url);
    if (!parsed) return "";
    return parsed->scheme + "://" + parsed->hostPort + parsed->path;
}

std::string UrlUtils::origin(const std::string& url) {
    auto parsed = parse(url);
    if (!parsed) return "";
    return parsed->scheme + "://" + parsed->hostPort;
}

bool UrlUtils::samePage(const std::string& a, const std::string& b, bool includeQuery) {
    auto pa = parse(a);
    auto pb = parse(b);
    if (!pa || !pb) return false;
    if (pa->scheme != pb->scheme || pa->hostPort != pb->hostPort) return false;
    if (pa->path != pb->path) return false;
    if (includeQuery && pa->query != pb->query) return false;
    return true;
}

bool UrlUtils::matches(const std::string& requested, const std::string& liveUrl,
                       TargetMatchRule rule) {
    std::string req = normalize(requested);
    std::string live = normalize(liveUrl);
    if (req.empty() || live.empty()) return false;

    switch (rule) {
        case TargetMatchRule::Exact:
            return req == live;
        case TargetMatchRule::UrlPrefix:
            return live.rfind(req, 0) == 0;
        case TargetMatchRule::OriginPath:
            return originAndPath(req) == originAndPath(live);
    }
    return false;
}

} // namespace edgemon
