#include "trading/ocr/OcrTypes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <sstream>
#include <unordered_set>

namespace trading {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace

OcrContentType OcrClassifier::classify(const std::string& text,
                                       const std::string& pageRole) const {
    std::string t = lower(text);

    // Alert-ish keywords always win (safety-relevant).
    static const char* kAlert[] = {
        "liquidation", "margin call", "stop loss", "take profit", "warning",
        "insufficient", "rejected", "filled", "killed", "tp hit", "sl hit"
    };
    for (const char* k : kAlert)
        if (t.find(k) != std::string::npos) return OcrContentType::ALERT;

    // Economic-calendar markers (highest precedence among content types:
    // "CPI" alone on a news page is an economic event, not a news story).
    static const char* kEcon[] = {
        "cpi", "unemployment", "nonfarm", "interest rate", "fomc",
        "ecb", "gdp", "pmi", "retail sales", "central bank"
    };
    int econHits = 0;
    for (const char* k : kEcon)
        if (t.find(k) != std::string::npos) ++econHits;

    // News headlines.
    static const char* kNews[] = {
        "breaking", "reuters", "bloomberg", "reports", "announces", "surges",
        "plunges", "soars", "tumbles", "etf", "fed", "rate decision",
        "inflation", "jobs report", "rally", "selloff"
    };
    int newsHits = 0;
    for (const char* k : kNews)
        if (t.find(k) != std::string::npos) ++newsHits;

    if (econHits > 0 && econHits >= newsHits) return OcrContentType::ECONOMIC;
    if (newsHits > 0) return OcrContentType::NEWS;

    // Words like "surge/drop/gain" with a percentage or price signal.
    static const char* kMove[] = {"surge", "plunge", "gain", "drop", "jump",
                                  "fall", "rise", "up ", "down ", "higher",
                                  "lower", "soar", "tumble"};
    for (const char* k : kMove)
        if (t.find(k) != std::string::npos) return OcrContentType::NEWS;

    // Role is a fallback domain hint, never an override.
    if (pageRole == "news") return OcrContentType::NEWS;
    if (pageRole == "alert") return OcrContentType::ALERT;
    if (pageRole == "price") return OcrContentType::PRICE;
    if (pageRole == "chart") return OcrContentType::CHART;
    return OcrContentType::UNKNOWN;
}

std::vector<std::string> OcrClassifier::extractSymbols(const std::string& text) const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::string t = text;
    // Uppercase tokens that look like tickers (2-6 alnum, may contain -).
    std::string tok;
    auto flush = [&]() {
        if (tok.size() < 2 || tok.size() > 8) { tok.clear(); return; }
        bool hasDigit = false, hasAlpha = false;
        for (char c : tok) {
            if (std::isdigit(static_cast<unsigned char>(c))) hasDigit = true;
            if (std::isalpha(static_cast<unsigned char>(c))) hasAlpha = true;
        }
        // Require at least one letter and (usually) all-caps-ish.
        if (!hasAlpha) { tok.clear(); return; }
        bool upper = true;
        for (char c : tok)
            if (std::islower(static_cast<unsigned char>(c))) upper = false;
        // Skip common words that appear uppercase in headlines.
        static const std::unordered_set<std::string> skip = {
            "THE", "AND", "FOR", "WITH", "FROM", "THAT", "THIS", "INTO",
            "AFTER", "BEFORE", "US", "UK", "EU", "ETF", "CEO", "Q1", "Q2",
            "Q3", "Q4", "USD", "EUR", "GBP", "JPY", "NEW", "RECORD", "HIGH",
            "LOW", "YEAR", "DAY", "MONTH", "FED", "CPI"
        };
        if (skip.count(tok) == 0 && upper) seen.insert(tok);
        tok.clear();
    };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '/') {
            tok.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        } else {
            flush();
        }
    }
    flush();
    out.assign(seen.begin(), seen.end());
    std::sort(out.begin(), out.end());
    return out;
}

double OcrClassifier::sentiment(const std::string& text) const {
    std::string t = lower(text);
    static const char* kPos[] = {"surge", "soar", "gain", "rally", "jump",
                                 "beat", "upgrade", "bullish", "outperform",
                                 "positive", "record", "breakout", "inflow"};
    static const char* kNeg[] = {"plunge", "tumble", "drop", "selloff", "crash",
                                 "miss", "downgrade", "bearish", "underperform",
                                 "negative", "outflow", "recession", "default"};
    double score = 0.0;
    for (const char* w : kPos)
        if (t.find(w) != std::string::npos) score += 0.2;
    for (const char* w : kNeg)
        if (t.find(w) != std::string::npos) score -= 0.2;
    if (score > 1.0) score = 1.0;
    if (score < -1.0) score = -1.0;
    return score;
}

bool OcrClassifier::isUrgent(const std::string& text) const {
    std::string t = lower(text);
    static const char* kUrgent[] = {"breaking", "liquidation", "margin call",
                                    "flash", "halt", "emergency", "alert"};
    for (const char* w : kUrgent)
        if (t.find(w) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// OcrDeduplicator
// ---------------------------------------------------------------------------

bool OcrDeduplicator::isNew(const std::string& source,
                            const std::string& normalizedText,
                            uint64_t nowNs, uint64_t windowNs) {
    uint64_t h = fnv1a(source) ^ (fnv1a(normalizedText) * 1099511628211ULL);
    // Purge old entries.
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                 [nowNs, windowNs](const Entry& e) {
                                     return nowNs - e.seenAt > windowNs;
                                 }),
                  recent_.end());
    for (const auto& e : recent_) {
        if (e.hash == h) return false;
    }
    recent_.push_back(Entry{nowNs, h});
    if (recent_.size() > 4096) recent_.erase(recent_.begin());
    return true;
}

void OcrDeduplicator::clear() {
    recent_.clear();
}

} // namespace trading
