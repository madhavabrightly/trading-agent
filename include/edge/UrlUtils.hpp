#pragma once

// Shared URL normalization + deterministic target-match helpers. Kept free of
// Edge/CDP dependencies so resolver, registry, report naming and tests can all
// use the same logic without sockets.

#include "core/Types.hpp"
#include <string>
#include <optional>

namespace edgemon {

struct ParsedUrl {
    std::string scheme;      // "http" | "https" (lowercase)
    std::string host;        // lowercase, no port
    std::string hostPort;    // lowercase host + :port (only when non-default)
    std::string path;        // includes leading '/'
    std::string query;       // without '?'
    std::string fragment;    // without '#'
};

class UrlUtils {
public:
    // Best-effort parse. Returns nullopt for clearly invalid / non-http URLs.
    static std::optional<ParsedUrl> parse(const std::string& rawUrl);

    // Normalize for matching: scheme+host lowercase, default port stripped,
    // fragment removed, trailing slash removed (except root), scheme added when
    // the input looks like a bare host/path. Empty on failure.
    static std::string normalize(std::string url);

    // scheme://host[:port]/path  (query+fragment stripped).
    static std::string originAndPath(const std::string& url);

    // scheme://host[:port]
    static std::string origin(const std::string& url);

    // True when both parse and the scheme+host+path match exactly (query is
    // ignored unless includeQuery is set).
    static bool samePage(const std::string& a, const std::string& b,
                         bool includeQuery = true);

    // Rule-based matching against a single live tab URL.
    static bool matches(const std::string& requested, const std::string& liveUrl,
                        TargetMatchRule rule);
};

} // namespace edgemon
