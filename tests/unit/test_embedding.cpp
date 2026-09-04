#include "embedding/EmbeddingEngine.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <cmath>

using namespace edgemon;

void test_embedding_result_structure() {
    std::cout << "Testing EmbeddingResult structure..." << std::endl;
    
    EmbeddingResult result;
    result.id = "emb-001";
    result.targetId = TargetId("test-001");
    result.content = "BTC: $104,250";
    result.vector.resize(128, 0.5f);
    result.timestamp = 1234567890;
    result.url = PageUrl("https://example.com");
    result.source = ContentSource::OCR;
    result.confidence = 0.95f;
    result.contentHash = 987654321;
    
    assert(result.id == "emb-001");
    assert(result.targetId.value == "test-001");
    assert(result.vector.size() == 128);
    assert(result.source == ContentSource::OCR);
    
    std::cout << "  EmbeddingResult structure: PASS" << std::endl;
}

void test_embedding_config() {
    std::cout << "Testing EmbeddingConfig..." << std::endl;
    
    EmbeddingConfig config;
    config.modelPath = "/models/bge-small";
    config.modelName = "BGE-small-en-v1.5";
    config.embeddingDim = 384;
    config.batchSize = 16;
    config.maxSequenceLength = 256;
    config.normalize = true;
    
    assert(config.modelName == "BGE-small-en-v1.5");
    assert(config.embeddingDim == 384);
    assert(config.batchSize == 16);
    
    std::cout << "  EmbeddingConfig: PASS" << std::endl;
}

void test_embedding_engine_creation() {
    std::cout << "Testing EmbeddingEngine creation..." << std::endl;
    
    auto engine = std::make_unique<EmbeddingEngine>();
    assert(!engine->isInitialized());
    
    std::cout << "  EmbeddingEngine creation: PASS" << std::endl;
}

void test_embedding_engine_init() {
    std::cout << "Testing EmbeddingEngine initialization..." << std::endl;
    
    auto engine = std::make_unique<EmbeddingEngine>();
    
    EmbeddingConfig config;
    config.modelName = "BGE-small-en-v1.5";
    config.embeddingDim = 384;
    
    bool success = engine->initialize(config);
    assert(success);
    assert(engine->isInitialized());
    assert(engine->getDimension() == 384);
    assert(engine->getModelName() == "BGE-small-en-v1.5");
    
    engine->shutdown();
    
    std::cout << "  EmbeddingEngine initialization: PASS" << std::endl;
}

void test_embedding_generation() {
    std::cout << "Testing embedding generation..." << std::endl;
    
    auto engine = std::make_unique<EmbeddingEngine>();
    
    EmbeddingConfig config;
    config.embeddingDim = 128;
    engine->initialize(config);
    
    std::string text = "BTC price is $104,250";
    auto embedding = engine->embed(text);
    
    assert(embedding.size() == 128);
    
    float sum = 0.0f;
    for (float v : embedding) {
        sum += v * v;
    }
    float norm = std::sqrt(sum);
    assert(std::abs(norm - 1.0f) < 0.01f);
    
    auto sameText = engine->embed(text);
    assert(embedding == sameText);
    
    auto differentText = engine->embed("ETH price is $3,450");
    assert(embedding != differentText);
    
    engine->shutdown();
    
    std::cout << "  Embedding generation: PASS" << std::endl;
}

void test_embedding_batch() {
    std::cout << "Testing embedding batch..." << std::endl;
    
    auto engine = std::make_unique<EmbeddingEngine>();
    
    EmbeddingConfig config;
    config.embeddingDim = 64;
    config.batchSize = 4;
    engine->initialize(config);
    
    std::vector<std::string> texts = {
        "BTC: $104,250",
        "ETH: $3,450",
        "SOL: $180",
        "XRP: $0.58"
    };
    
    std::vector<std::vector<float>> results;
    engine->embedBatch(texts, results);
    
    assert(results.size() == 4);
    for (const auto& emb : results) {
        assert(emb.size() == 64);
    }
    
    auto stats = engine->getStats();
    assert(stats.batchCount == 1);
    
    engine->shutdown();
    
    std::cout << "  Embedding batch: PASS" << std::endl;
}

void test_embedding_store_creation() {
    std::cout << "Testing EmbeddingStore creation..." << std::endl;
    
    auto store = std::make_unique<EmbeddingStore>();
    
    bool success = store->initialize("localhost:6334", "test.db");
    assert(success);
    
    assert(store->count() == 0);
    
    store->shutdown();
    
    std::cout << "  EmbeddingStore creation: PASS" << std::endl;
}

void test_embedding_store_add() {
    std::cout << "Testing EmbeddingStore add..." << std::endl;
    
    auto store = std::make_unique<EmbeddingStore>();
    store->initialize();
    
    EmbeddingResult emb1;
    emb1.id = "emb-001";
    emb1.targetId = TargetId("target-A");
    emb1.content = "BTC: $104,250";
    emb1.vector.resize(128, 0.1f);
    emb1.timestamp = 1000;
    emb1.contentHash = 12345;
    
    assert(store->add(emb1));
    assert(store->count() == 1);
    
    EmbeddingResult emb2;
    emb2.id = "emb-002";
    emb2.targetId = TargetId("target-A");
    emb2.content = "ETH: $3,450";
    emb2.vector.resize(128, 0.2f);
    emb2.timestamp = 1001;
    emb2.contentHash = 67890;
    
    assert(store->add(emb2));
    assert(store->count() == 2);
    
    store->shutdown();
    
    std::cout << "  EmbeddingStore add: PASS" << std::endl;
}

void test_embedding_store_duplicate() {
    std::cout << "Testing EmbeddingStore duplicate detection..." << std::endl;
    
    auto store = std::make_unique<EmbeddingStore>();
    store->initialize();
    
    EmbeddingResult emb;
    emb.id = "emb-001";
    emb.targetId = TargetId("target-A");
    emb.content = "BTC: $104,250";
    emb.vector.resize(64, 0.5f);
    emb.contentHash = 99999;
    
    assert(store->add(emb));
    assert(store->count() == 1);
    
    EmbeddingResult dup;
    dup.id = "emb-002";
    dup.targetId = TargetId("target-A");
    dup.content = "BTC: $104,250";
    dup.vector.resize(64, 0.5f);
    dup.contentHash = 99999;
    
    assert(!store->add(dup));
    assert(store->count() == 1);
    
    store->shutdown();
    
    std::cout << "  EmbeddingStore duplicate: PASS" << std::endl;
}

void test_embedding_store_search() {
    std::cout << "Testing EmbeddingStore search..." << std::endl;
    
    auto store = std::make_unique<EmbeddingStore>();
    store->initialize();
    
    auto engine = std::make_shared<EmbeddingEngine>();
    EmbeddingConfig config;
    config.embeddingDim = 64;
    engine->initialize(config);
    
    std::vector<std::pair<std::string, std::string>> content = {
        {"BTC: $104,250", "target-A"},
        {"ETH: $3,450", "target-A"},
        {"SOL: $180", "target-B"},
        {"XRP: $0.58", "target-B"}
    };
    
    for (size_t i = 0; i < content.size(); ++i) {
        EmbeddingResult emb;
        emb.id = "emb-" + std::to_string(i);
        emb.targetId = TargetId(content[i].second);
        emb.content = content[i].first;
        emb.vector = engine->embed(content[i].first);
        emb.contentHash = i + 1000;
        store->add(emb);
    }
    
    auto results = store->search("crypto price", 3);
    assert(!results.empty());
    
    auto targetResults = store->search("crypto", TargetId("target-A"), 3);
    assert(targetResults.size() <= 3);
    for (const auto& r : targetResults) {
        assert(r.targetId.value == "target-A");
    }
    
    engine->shutdown();
    store->shutdown();
    
    std::cout << "  EmbeddingStore search: PASS" << std::endl;
}

void test_incremental_pipeline() {
    std::cout << "Testing IncrementalEmbeddingPipeline..." << std::endl;
    
    auto engine = std::make_shared<EmbeddingEngine>();
    EmbeddingConfig config;
    config.embeddingDim = 64;
    engine->initialize(config);
    
    auto store = std::make_shared<EmbeddingStore>();
    store->setEngine(engine);  // same engine for store + query embedding
    store->initialize();
    
    IncrementalEmbeddingPipeline pipeline;
    pipeline.setEmbeddingEngine(engine);
    pipeline.setEmbeddingStore(store);
    
    pipeline.start();
    
    TemporalObservation obs;
    obs.targetId = TargetId("test-target");
    obs.timestamp = 1234567890;
    obs.contentHash = 54321;
    
    pipeline.addText("BTC: $104,250", obs);
    obs.contentHash = 54322;
    pipeline.addText("ETH: $3,450", obs);
    
    pipeline.processNow();
    
    // The background worker also drains the queue, so poll for completion.
    for (int i = 0; i < 100 && store->count() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pipeline.processNow();
    }
    assert(store->count() == 2);
    
    // Re-adding the first text (with its original hash) must be deduplicated.
    obs.contentHash = 54321;
    pipeline.addText("BTC: $104,250", obs);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pipeline.processNow();
    
    assert(store->count() == 2);
    
    pipeline.stop();
    store->shutdown();
    engine->shutdown();
    
    std::cout << "  IncrementalEmbeddingPipeline: PASS" << std::endl;
}

void test_cosine_similarity() {
    std::cout << "Testing cosine similarity via search..." << std::endl;
    
    auto engine = std::make_shared<EmbeddingEngine>();
    EmbeddingConfig config;
    config.embeddingDim = 32;
    engine->initialize(config);
    
    auto store = std::make_shared<EmbeddingStore>();
    store->setEngine(engine);  // same engine for store + query embedding
    store->initialize();
    
    // Same content must return itself with ~1.0 score; unrelated content must
    // score lower. This exercises the internal cosine-similarity path publicly.
    EmbeddingResult e1;
    e1.id = "sim-1";
    e1.content = "bitcoin price today";
    e1.vector = engine->embed("bitcoin price today");
    e1.contentHash = std::hash<std::string>{}(e1.content);
    store->add(e1);
    
    EmbeddingResult e2;
    e2.id = "sim-2";
    e2.content = "quantum computing research";
    e2.vector = engine->embed("quantum computing research");
    e2.contentHash = std::hash<std::string>{}(e2.content);
    store->add(e2);
    
    auto identical = store->search("bitcoin price today", 1);
    assert(!identical.empty());
    assert(identical[0].id == "sim-1");
    
    auto unrelated = store->search("quantum computing research", 1);
    assert(!unrelated.empty());
    assert(unrelated[0].id == "sim-2");
    
    store->shutdown();
    engine->shutdown();
    
    std::cout << "  Cosine similarity via search: PASS" << std::endl;
}

void test_incremental_skip_unchanged() {
    std::cout << "Testing incremental skip unchanged..." << std::endl;
    
    auto engine = std::make_shared<EmbeddingEngine>();
    EmbeddingConfig config;
    config.embeddingDim = 32;
    engine->initialize(config);
    
    auto store = std::make_shared<EmbeddingStore>();
    store->setEngine(engine);  // same engine for store + query embedding
    store->initialize();
    
    IncrementalEmbeddingPipeline pipeline;
    pipeline.setEmbeddingEngine(engine);
    pipeline.setEmbeddingStore(store);
    pipeline.start();
    
    std::vector<std::pair<std::string, uint64_t>> sequence = {
        {"BTC: $104,200", 1000},
        {"BTC: $104,200", 1001},
        {"BTC: $104,200", 1002},
        {"BTC: $104,250", 1003},
        {"BTC: $104,250", 1004},
        {"BTC: $104,260", 1005},
    };
    
    for (const auto& [text, ts] : sequence) {
        TemporalObservation obs;
        obs.targetId = TargetId("btc-tracker");
        obs.timestamp = ts;
        // Same text -> same hash: the pipeline must skip the unchanged repeats
        // and index exactly one vector per unique text.
        obs.contentHash = std::hash<std::string>{}(text);
        pipeline.addText(text, obs);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pipeline.processNow();
    
    auto autoStats = pipeline.getStoreStats();
    
    assert(autoStats.totalVectors == 3);
    
    pipeline.stop();
    store->shutdown();
    engine->shutdown();
    
    std::cout << "  Incremental skip unchanged: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - Embedding Engine Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_embedding_result_structure();
    test_embedding_config();
    test_embedding_engine_creation();
    test_embedding_engine_init();
    test_embedding_generation();
    test_embedding_batch();
    test_embedding_store_creation();
    test_embedding_store_add();
    test_embedding_store_duplicate();
    test_embedding_store_search();
    test_incremental_pipeline();
    test_cosine_similarity();
    test_incremental_skip_unchanged();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All embedding engine tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
