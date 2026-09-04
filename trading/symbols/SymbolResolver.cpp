#include "trading/symbols/SymbolResolver.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>

namespace trading {

namespace {

std::string join(const std::string& a, const std::string& b) {
    return a + "\x1F" + b;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

// Strips common quote suffixes from a crypto base.
std::string stripQuote(const std::string& s) {
    static const char* kQuotes[] = {"USDT", "USDC", "USD", "BUSD", "TUSD"};
    for (const char* q : kQuotes) {
        size_t ql = std::char_traits<char>::length(q);
        if (s.size() > ql && s.compare(s.size() - ql, ql, q) == 0)
            return s.substr(0, s.size() - ql);
    }
    return s;
}

} // namespace

void SymbolResolver::addAlias(const SymbolAlias& alias) {
    if (alias.exchange.empty() || alias.native.empty() || alias.canonical.empty())
        return;
    nativeToCanonical_[join(upper(alias.exchange), upper(alias.native))] =
        alias.canonical;
    canonicalToNative_[join(upper(alias.exchange), upper(alias.canonical))] =
        alias.native;
    aliases_.push_back(alias);
}

bool SymbolResolver::loadFile(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        error = "cannot open symbols file: " + path;
        return false;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        error = std::string("symbols parse error: ") + e.what();
        return false;
    }
    if (!j.contains("aliases") || !j["aliases"].is_array()) {
        error = "symbols file missing 'aliases' array";
        return false;
    }
    for (const auto& e : j["aliases"]) {
        if (!e.is_object()) continue;
        SymbolAlias a;
        a.exchange = e.value("exchange", "");
        a.native = e.value("native", "");
        a.canonical = e.value("canonical", "");
        a.kind = e.value("kind", "");
        if (!a.exchange.empty() && !a.native.empty() && !a.canonical.empty())
            addAlias(a);
    }
    return true;
}

bool SymbolResolver::looksCanonical(const std::string& s) {
    // "BASE/QUOTE" with a single slash.
    auto slash = s.find('/');
    return slash != std::string::npos && slash == s.rfind('/') &&
           slash > 0 && slash + 1 < s.size();
}

std::string SymbolResolver::normalizeHeuristic(const std::string& raw) const {
    std::string s = upper(raw);
    if (looksCanonical(s)) return s;
    // Remove common separators.
    std::string t;
    for (char c : s) {
        if (c == '-' || c == '_' || c == ' ') continue;
        t.push_back(c);
    }
    if (t.empty()) return raw;

    // Metal / forex canonical quotes
    struct Rule {
        const char* prefix;
        const char* canonical;
    };
    static const Rule kRules[] = {
        {"XAU", "XAU/USD"}, {"XAG", "XAG/USD"},
        {"GOLD", "XAU/USD"}, {"SILVER", "XAG/USD"},
        {"XPT", "XPT/USD"}, {"XPD", "XPD/USD"},
    };
    for (const auto& r : kRules) {
        if (t == r.prefix) return r.canonical;
        size_t pl = std::char_traits<char>::length(r.prefix);
        if (t.compare(0, pl, r.prefix) == 0) {
            // XAUUSD -> XAU/USD
            std::string rest = t.substr(pl);
            if (rest == "USD") return r.canonical;
        }
    }
    // Crypto bases with XBT alias handled BEFORE the forex rule (XBTUSD is 6
    // letters and would otherwise parse as forex base "XBT").
    if (t.size() > 3 && t.compare(0, 3, "XBT") == 0 &&
        t.compare(3, std::string::npos, "USD") == 0)
        return "BTC/USD";

    // Forex: 6 letters ending in a known quote
    static const char* kForexQuotes[] = {"USD", "EUR", "GBP", "JPY", "CHF",
                                         "AUD", "CAD", "NZD"};
    if (t.size() == 6) {
        for (const char* q : kForexQuotes) {
            size_t ql = std::char_traits<char>::length(q);
            if (t.compare(t.size() - ql, ql, q) == 0) {
                std::string base = t.substr(0, t.size() - ql);
                return base + "/" + q;
            }
        }
    }
    // Crypto: BTCUSDT/BTC-USD -> BTC/USD
    static const char* kCryptoQuotes[] = {"USDT", "USDC", "BUSD", "USD"};
    for (const char* q : kCryptoQuotes) {
        size_t ql = std::char_traits<char>::length(q);
        if (t.size() > ql && t.compare(t.size() - ql, ql, q) == 0) {
            std::string base = stripQuote(t);
            if (base == "XBT") base = "BTC";
            return base + "/USD";
        }
    }
    // Stocks: pure equity ticker -> TICKER/USD (best-effort)
    bool allAlpha = std::all_of(t.begin(), t.end(), [](unsigned char c) {
        return std::isalpha(c) != 0;
    });
    if (allAlpha && t.size() <= 6) return t + "/USD";
    return raw;
}

std::optional<std::string> SymbolResolver::canonical(
    const std::string& exchange, const std::string& native) const {
    auto it = nativeToCanonical_.find(join(upper(exchange), upper(native)));
    if (it != nativeToCanonical_.end()) return it->second;
    auto h = normalizeHeuristic(native);
    if (looksCanonical(h)) return h;
    return std::nullopt;
}

std::optional<std::string> SymbolResolver::native(
    const std::string& exchange, const std::string& canonicalSymbol) const {
    auto it = canonicalToNative_.find(join(upper(exchange), upper(canonicalSymbol)));
    if (it != canonicalToNative_.end()) return it->second;
    return std::nullopt;
}

} // namespace trading
