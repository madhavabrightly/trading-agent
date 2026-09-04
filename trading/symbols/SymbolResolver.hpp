#pragma once

// trading/symbols/SymbolResolver.hpp — maps broker-specific tickers to a
// canonical asset (spec §10). Exchange-specific mappings live in symbols.json.

#include <string>
#include <vector>
#include <optional>
#include <map>

namespace trading {

// One mapping entry for a broker's symbol representation.
struct SymbolAlias {
    std::string exchange;     // "alpaca", "binance", ...
    std::string native;       // "BTCUSD", "BTC-USD", "XBTUSD"
    std::string canonical;    // "BTC/USD"
    std::string kind;         // "crypto" | "stock" | "forex" | "metal" | "index"
};

class SymbolResolver {
public:
    SymbolResolver() = default;

    // Loads extra mappings from a JSON file with schema:
    //   { "aliases": [ {"exchange","native","canonical","kind"}, ... ] }
    bool loadFile(const std::string& path, std::string& error);

    // Registers a single alias at runtime.
    void addAlias(const SymbolAlias& alias);

    // Canonical asset for a broker-native symbol. Returns nullopt if unknown.
    std::optional<std::string> canonical(const std::string& exchange,
                                         const std::string& native) const;

    // The native symbol a given exchange expects for a canonical asset.
    std::optional<std::string> native(const std::string& exchange,
                                      const std::string& canonical) const;

    // Heuristic fallback for common forms (no mapping required):
    // BTCUSDT / BTC-USD / XBTUSD -> BTC/USD ; XAUUSD/GOLD -> XAU/USD ; AAPL -> AAPL/USD
    std::string normalizeHeuristic(const std::string& raw) const;

    // True when the string already looks canonical ("BASE/QUOTE").
    static bool looksCanonical(const std::string& s);

    bool empty() const { return aliases_.empty(); }
    size_t size() const { return aliases_.size(); }

private:
    // key = exchange + "\x1F" + native  ->  canonical
    std::map<std::string, std::string> nativeToCanonical_;
    // key = exchange + "\x1F" + canonical -> native
    std::map<std::string, std::string> canonicalToNative_;
    std::vector<SymbolAlias> aliases_;
};

} // namespace trading
