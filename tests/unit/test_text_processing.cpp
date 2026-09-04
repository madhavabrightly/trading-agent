#include "extraction/TextProcessor.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace edgemon;

void test_text_normalizer_basic() {
    std::cout << "Testing TextNormalizer basic..." << std::endl;
    
    TextNormalizer normalizer;
    
    std::string text = "  BTC:   $104,250  ";
    std::string result = normalizer.normalize(text);
    
    assert(!result.empty());
    assert(result.find("  ") == std::string::npos);
    
    std::cout << "  TextNormalizer basic: PASS" << std::endl;
}

void test_text_normalizer_numbers() {
    std::cout << "Testing TextNormalizer numbers preservation..." << std::endl;
    
    TextNormalizer normalizer;
    
    std::string text = "BTC: $104,250";
    std::string result = normalizer.normalize(text);
    
    // Canonical normalizer collapses whitespace/URLs/emails but keeps
    // punctuation and digits intact.
    assert(result.find("104,250") != std::string::npos);
    assert(result.find("$") != std::string::npos);
    assert(result.find("  ") == std::string::npos);
    
    std::cout << "  TextNormalizer numbers: PASS" << std::endl;
}

void test_text_normalizer_hash() {
    std::cout << "Testing TextNormalizer hash..." << std::endl;
    
    TextNormalizer normalizer;
    
    std::string text1 = "BTC: $104,250";
    std::string text2 = "BTC: $104,250";
    std::string text3 = "ETH: $3,450";
    
    std::string hash1 = normalizer.computeHash(text1);
    std::string hash2 = normalizer.computeHash(text2);
    std::string hash3 = normalizer.computeHash(text3);
    
    assert(hash1 == hash2);
    assert(hash1 != hash3);
    
    std::cout << "  TextNormalizer hash: PASS" << std::endl;
}

void test_text_chunker() {
    std::cout << "Testing TextChunker..." << std::endl;
    
    TextProcessingConfig config;
    config.maxChunkSize = 100;
    config.minChunkSize = 10;
    
    TextChunker chunker(config);
    
    std::string text = "This is a long piece of text that should be chunked into smaller pieces for processing.";
    
    auto chunks = chunker.chunk(text, TargetId("test-001"), PageUrl("https://example.com"), ContentSource::DOM);
    
    assert(!chunks.empty());
    
    std::cout << "  TextChunker: PASS (created " << chunks.size() << " chunks)" << std::endl;
}

void test_deduplicator_basic() {
    std::cout << "Testing Deduplicator basic..." << std::endl;
    
    Deduplicator dedup;
    
    std::string text1 = "BTC: $104,250";
    std::string text2 = "BTC: $104,250";
    std::string text3 = "ETH: $3,450";
    
    // Deduplicator is check-then-mark (process() calls isDuplicate followed by
    // markSeen). Use an explicit hash function so check and mark agree.
    dedup.setHashFunction([](const std::string& s) { return std::hash<std::string>{}(s); });
    assert(!dedup.isDuplicate(text1));
    dedup.markSeen(text1, std::hash<std::string>{}(text1));
    assert(dedup.isDuplicate(text2));
    assert(!dedup.isDuplicate(text3));
    
    auto stats = dedup.getStats();
    assert(stats.totalChecked == 3);
    assert(stats.duplicates == 1);
    assert(stats.unique == 2);
    
    std::cout << "  Deduplicator basic: PASS" << std::endl;
}

void test_deduplicator_temporal() {
    std::cout << "Testing Deduplicator temporal..." << std::endl;
    
    Deduplicator dedup;
    
    std::vector<std::pair<std::string, uint64_t>> sequence = {
        {"BTC: $104,200", 1000},
        {"BTC: $104,200", 1001},
        {"BTC: $104,200", 1002},
        {"BTC: $104,250", 1003},
        {"BTC: $104,250", 1004},
    };
    
    // Mark observations with timestamps so forgetOlderThan has data to evict.
    auto mark = [&dedup](const std::string& text, uint64_t ts) {
        TemporalObservation obs;
        obs.rawText = text;
        obs.timestamp = ts;
        obs.contentHash = std::hash<std::string>{}(text);
        dedup.markSeen(obs);
    };

    size_t uniqueCount = 0;
    for (const auto& [text, timestamp] : sequence) {
        if (!dedup.isDuplicate(text, std::hash<std::string>{}(text))) {
            uniqueCount++;
            mark(text, timestamp);
        }
    }
    
    assert(uniqueCount == 2);
    
    dedup.forgetOlderThan(1002);
    
    std::string reuse = "BTC: $104,200";
    bool canReuse = !dedup.isDuplicate(reuse, std::hash<std::string>{}(reuse));
    assert(canReuse);
    
    std::cout << "  Deduplicator temporal: PASS" << std::endl;
}

void test_temporal_store_add() {
    std::cout << "Testing TemporalStore add..." << std::endl;
    
    TemporalStore store;
    
    TemporalObservation obs1;
    obs1.targetId = TargetId("test-001");
    obs1.timestamp = 1000;
    obs1.rawText = "BTC: $104,250";
    obs1.normalizedText = "BTC: 104250";
    obs1.contentHash = 12345;
    
    std::string id1 = store.add(obs1);
    assert(!id1.empty());
    
    TemporalObservation obs2;
    obs2.targetId = TargetId("test-001");
    obs2.timestamp = 1001;
    obs2.rawText = "ETH: $3,450";
    obs2.normalizedText = "ETH: 3450";
    obs2.contentHash = 67890;
    
    std::string id2 = store.add(obs2);
    assert(!id2.empty());
    
    assert(store.count() == 2);
    
    std::cout << "  TemporalStore add: PASS" << std::endl;
}

void test_temporal_store_query() {
    std::cout << "Testing TemporalStore queries..." << std::endl;
    
    TemporalStore store;
    
    for (int i = 0; i < 5; i++) {
        TemporalObservation obs;
        obs.targetId = TargetId("target-A");
        obs.timestamp = 1000 + i;
        obs.rawText = "Price: " + std::to_string(100 + i);
        obs.contentHash = i;
        store.add(obs);
    }
    
    auto byTarget = store.getByTarget(TargetId("target-A"), 10);
    assert(byTarget.size() == 5);
    
    auto byTimeRange = store.getByTimeRange(1002, 1004, 10);
    assert(byTimeRange.size() == 3);
    
    auto recent = store.getRecent(3);
    assert(recent.size() == 3);
    
    std::cout << "  TemporalStore queries: PASS" << std::endl;
}

void test_temporal_store_history() {
    std::cout << "Testing TemporalStore history preservation..." << std::endl;
    
    TemporalStore store;
    
    std::vector<std::pair<std::string, uint64_t>> prices = {
        {"BTC: $104,200", 1000},
        {"BTC: $104,250", 1001},
        {"BTC: $104,190", 1002},
        {"BTC: $104,250", 1003},
        {"BTC: $104,260", 1004},
    };
    
    for (const auto& [text, ts] : prices) {
        TemporalObservation obs;
        obs.targetId = TargetId("btc-tracker");
        obs.timestamp = ts;
        obs.rawText = text;
        obs.contentHash = std::hash<std::string>{}(text);
        store.add(obs);
    }
    
    auto history = store.getByTarget(TargetId("btc-tracker"), 10);
    assert(history.size() == 5);
    
    // TemporalStore returns newest-first (recent observations at index 0).
    assert(history[0].rawText == "BTC: $104,260");
    assert(history[4].rawText == "BTC: $104,200");
    
    std::cout << "  TemporalStore history: PASS" << std::endl;
}

void test_temporal_observation_structure() {
    std::cout << "Testing TemporalObservation structure..." << std::endl;
    
    TemporalObservation obs;
    obs.id = "obs-001";
    obs.targetId = TargetId("test-001");
    obs.timestamp = 1234567890;
    obs.source = ContentSource::DOM;
    obs.rawText = "BTC: $104,250";
    obs.normalizedText = "BTC 104250";
    obs.confidence = 0.95f;
    obs.contentHash = 987654321;
    obs.isDuplicate = false;
    obs.isIndexed = true;
    
    assert(obs.id == "obs-001");
    assert(obs.targetId.value == "test-001");
    assert(obs.source == ContentSource::DOM);
    assert(obs.isDuplicate == false);
    assert(obs.isIndexed == true);
    
    std::cout << "  TemporalObservation structure: PASS" << std::endl;
}

void test_text_processing_pipeline() {
    std::cout << "Testing TextProcessingPipeline..." << std::endl;
    
    TextProcessingPipeline pipeline;
    
    TextProcessingConfig config;
    config.preserveNumbers = true;
    config.maxChunkSize = 100;
    config.minChunkSize = 5;
    pipeline.setNormalizerConfig(config);
    pipeline.setChunkerConfig(config);
    
    bool callbackCalled = false;
    pipeline.addObserver([&callbackCalled](const TemporalObservation& obs) {
        callbackCalled = true;
    });
    
    auto result = pipeline.process("BTC: $104,250", TargetId("pipeline-test"), 
                                 PageUrl("https://example.com"), ContentSource::DOM);
    
    assert(callbackCalled);
    assert(!result.isDuplicate);
    assert(result.isIndexed);
    
    std::cout << "  TextProcessingPipeline: PASS" << std::endl;
}

void test_deduplication_sequence() {
    std::cout << "Testing deduplication sequence..." << std::endl;
    
    TextProcessingPipeline pipeline;
    pipeline.setDeduplicationEnabled(true);
    
    std::vector<std::pair<std::string, bool>> expected = {
        {"BTC: $104,200", true},
        {"BTC: $104,200", false},
        {"BTC: $104,200", false},
        {"BTC: $104,250", true},
        {"BTC: $104,250", false},
        {"ETH: $3,450", true},
    };
    
    size_t uniqueCount = 0;
    for (const auto& [text, expectUnique] : expected) {
        auto result = pipeline.process(text, TargetId("dedup-test"), 
                                    PageUrl("https://example.com"), ContentSource::DOM);
        if (!result.isDuplicate) uniqueCount++;
        assert(result.isDuplicate == !expectUnique);
    }
    
    assert(uniqueCount == 3);
    
    std::cout << "  Deduplication sequence: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - Text Processing Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_text_normalizer_basic();
    test_text_normalizer_numbers();
    test_text_normalizer_hash();
    test_text_chunker();
    test_deduplicator_basic();
    test_deduplicator_temporal();
    test_temporal_store_add();
    test_temporal_store_query();
    test_temporal_store_history();
    test_temporal_observation_structure();
    test_text_processing_pipeline();
    test_deduplication_sequence();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All text processing tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
