#pragma once

// trading/ocr/OcrTypes.hpp — OCR integration layer types. OcrEvent matches the
// spec (§3): every reading carries timestamp, source, confidence, sequence,
// region and raw text. OCR data is never trusted directly — classifier output
// is advisory and must pass validation/dedup before it can influence signals.

#include <cstdint>
#include <string>
#include <vector>

namespace trading {

// What a chunk of OCR text is classified as (spec §13). Advisory only.
enum class OcrContentType {
    PRICE,
    NEWS,
    ECONOMIC,
    SENTIMENT,
    ALERT,
    CHART,
    PLATFORM,   // exchange page chrome: balances, fees, funding
    UNKNOWN
};

constexpr const char* toString(OcrContentType t) {
    switch (t) {
        case OcrContentType::PRICE:    return "price";
        case OcrContentType::NEWS:     return "news";
        case OcrContentType::ECONOMIC: return "economic";
        case OcrContentType::SENTIMENT:return "sentiment";
        case OcrContentType::ALERT:    return "alert";
        case OcrContentType::CHART:    return "chart";
        case OcrContentType::PLATFORM: return "platform";
        case OcrContentType::UNKNOWN:  return "unknown";
    }
    return "unknown";
}

// Raw OCR reading emitted by the adapter into the event bus (spec §3).
struct OcrEvent {
    std::string source;          // e.g. "edge:binance-trade" or target id
    std::string page;            // URL / page identity
    std::string region;          // detected region label (e.g. line bbox)
    std::string text;            // raw extracted text
    uint64_t timestampNs = 0;    // capture time, UTC epoch ns
    double confidence = 0.0;     // 0..1 from the recognizer
    uint64_t sequence = 0;       // monotonic per-source

    // --- filled by the classifier, not by OCR ---
    OcrContentType type = OcrContentType::UNKNOWN;
    std::vector<std::string> symbols;    // canonical symbol hints if any
    double sentiment = 0.0;              // -1..1 when SENTIMENT/NEWS
    bool urgent = false;
    std::string kind;                    // "price", "news", "alert", "econ"
};

// Classifier turns raw OCR text into a structured hint (spec §13). Pure
// function of the text + page role; stateless so it is trivially testable.
class OcrClassifier {
public:
    OcrContentType classify(const std::string& text,
                            const std::string& pageRole) const;

    // Extract possible tickers/symbols from text (canonical form best-effort).
    std::vector<std::string> extractSymbols(const std::string& text) const;

    // Coarse sentiment on a -1..1 scale (keyword lexicon).
    double sentiment(const std::string& text) const;

    // True when the text looks like a market-moving headline / alert.
    bool isUrgent(const std::string& text) const;
};

// Content-hash + window dedup for OCR/news repeats (spec §14). Thread-safe.
class OcrDeduplicator {
public:
    OcrDeduplicator() = default;

    // Returns false when this text was seen recently (within window) for the
    // same source; otherwise records and returns true.
    bool isNew(const std::string& source, const std::string& normalizedText,
               uint64_t nowNs, uint64_t windowNs = 300'000'000'000ULL);  // 5 min

    void clear();

private:
    struct Entry {
        uint64_t seenAt = 0;
        uint64_t hash = 0;
    };
    std::vector<Entry> recent_;
};

} // namespace trading
