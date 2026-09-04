// Unit tests for OCR classification + dedup (Phase 8).
#include "trading/ocr/OcrTypes.hpp"
#include <cstdio>
#include <cmath>

using namespace trading;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

int main() {
    OcrClassifier clf;

    // Price page role
    CHECK(clf.classify("45,000.50", "price") == OcrContentType::PRICE,
          "price page text -> PRICE");

    // News detection
    CHECK(clf.classify("BREAKING: BTC ETF inflows reach $1.2B", "news") ==
              OcrContentType::NEWS,
          "breaking headline -> NEWS");
    CHECK(clf.classify("Fed rate decision sends stocks lower", "chart") ==
              OcrContentType::NEWS,
          "fed + lower -> NEWS even on chart page");

    // Alert detection
    CHECK(clf.classify("Margin call: insufficient balance", "news") ==
              OcrContentType::ALERT,
          "margin call -> ALERT");

    // Economic
    CHECK(clf.classify("US CPI inflation data due Friday", "news") ==
              OcrContentType::ECONOMIC,
          "CPI -> ECONOMIC");

    // Sentiment lexicon
    CHECK(clf.sentiment("BTC surges to record high") > 0.3, "positive sentiment");
    CHECK(clf.sentiment("stocks plunge in selloff") < -0.3, "negative sentiment");
    CHECK(std::fabs(clf.sentiment("the cat sat on mat")) < 1e-9,
          "neutral text -> ~0");

    // Urgency
    CHECK(clf.isUrgent("BREAKING NEWS flash"), "breaking flagged urgent");
    CHECK(!clf.isUrgent("a quiet update on nothing"), "calm text not urgent");

    // Symbol extraction
    auto syms = clf.extractSymbols("BTC surges past ETH as AAPL falls");
    bool hasBtc = false, hasEth = false, hasAapl = false;
    for (const auto& s : syms) {
        if (s == "BTC") hasBtc = true;
        if (s == "ETH") hasEth = true;
        if (s == "AAPL") hasAapl = true;
    }
    CHECK(hasBtc && hasEth && hasAapl, "extracts ticker-like symbols");

    // Dedup within window
    OcrDeduplicator dd;
    uint64_t now = 1'000'000'000'000ULL;  // arbitrary
    CHECK(dd.isNew("src1", "same text", now), "first sighting is new");
    CHECK(!dd.isNew("src1", "same text", now + 1'000'000'000ULL),
          "repeat within window deduped");
    CHECK(dd.isNew("src2", "same text", now + 2'000'000'000ULL),
          "different source not deduped");
    CHECK(dd.isNew("src1", "same text", now + 400'000'000'000ULL),
          "repeat after window is new again");

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
