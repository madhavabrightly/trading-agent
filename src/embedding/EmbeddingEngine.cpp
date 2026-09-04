#include "embedding/EmbeddingEngine.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace edgemon {

// ============================================================================
// EmbeddingEngine
// ============================================================================

EmbeddingEngine::EmbeddingEngine()
    : initialized_(false) {
}

EmbeddingEngine::~EmbeddingEngine() {
    shutdown();
}

bool EmbeddingEngine::initialize(const EmbeddingConfig& config) {
    if (initialized_.exchange(true)) {
        LOG_WARN("EmbeddingEngine already initialized");
        return true;
    }

    config_ = config;

    LOG_INFO("EmbeddingEngine initializing: {}", config.modelName);
    LOG_INFO("  Model path: {}", config.modelPath.empty() ? "(default)" : config.modelPath);
    LOG_INFO("  Embedding dimension: {}", config.embeddingDim);
    LOG_INFO("  Batch size: {}", config.batchSize);
    LOG_INFO("  Max sequence length: {}", config.maxSequenceLength);

    LOG_INFO("EmbeddingEngine initialized (placeholder - integrate ONNX Runtime for real inference)");
    return true;
}

void EmbeddingEngine::shutdown() {
    if (!initialized_.exchange(false)) {
        return;
    }

    if (onnxSession_) {
        onnxSession_ = nullptr;
    }

    if (tokenizer_) {
        tokenizer_ = nullptr;
    }

    LOG_INFO("EmbeddingEngine shutdown complete");
}

std::vector<float> EmbeddingEngine::embed(const std::string& text) {
    return embedSync(text);
}

std::vector<float> EmbeddingEngine::embedSync(const std::string& text) {
    if (!initialized_) {
        LOG_ERROR("EmbeddingEngine not initialized");
        return {};
    }

    if (text.empty()) {
        return std::vector<float>(config_.embeddingDim, 0.0f);
    }

    auto start = std::chrono::steady_clock::now();

    // Local bag-of-words embedding: tokenize on word + character boundaries so
    // lexical overlap drives semantic similarity. Uses double-hashing into the
    // output dimension (bloom-style) to reduce collision skew, and weights
    // exact words above character trigrams. Deterministic and dependency-free;
    // a real ONNX model would replace embedSync() without interface changes.
    std::vector<float> embedding(config_.embeddingDim, 0.0f);

    std::istringstream stream(text);
    std::string word;
    auto hashAt = [](const std::string& s, size_t dim) -> size_t {
        size_t h1 = std::hash<std::string>{}(s);
        size_t h2 = std::hash<std::string>{}(s + "#2");
        return (h1 ^ (h2 >> 1)) % dim;
    };

    while (stream >> word) {
        std::string lower;
        lower.reserve(word.size());
        for (char c : word) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower.empty()) continue;
        // Two buckets per word (bloom-style) to cut collision skew.
        size_t h1 = hashAt(lower, config_.embeddingDim);
        size_t h2 = hashAt(lower + "#", config_.embeddingDim);
        embedding[h1] += 1.0f;
        embedding[h2] += 1.0f;
        // Character trigram overlap gives partial-match signal.
        if (lower.size() >= 3) {
            for (size_t i = 0; i + 3 <= lower.size(); ++i) {
                size_t th = hashAt(lower.substr(i, 3), config_.embeddingDim);
                embedding[th] += 0.25f;
            }
        }
    }

    float sum = 0.0f;
    for (float v : embedding) {
        sum += v * v;
    }
    if (sum > 0.0f) {
        float norm = std::sqrt(sum);
        for (auto& v : embedding) {
            v /= norm;
        }
    }

    auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    totalEmbeddings_++;
    totalLatencyMs_ += elapsed;

    LOG_DEBUG("Embedding generated for '{}...' in {:.2f}ms", 
              text.substr(0, 20), elapsed);

    return embedding;
}

void EmbeddingEngine::embedBatch(const std::vector<std::string>& texts,
                                std::vector<std::vector<float>>& results) {
    if (!initialized_) {
        LOG_ERROR("EmbeddingEngine not initialized");
        return;
    }

    results.clear();
    results.reserve(texts.size());

    for (const auto& text : texts) {
        results.push_back(embed(text));
    }

    batchCount_++;
}

void EmbeddingEngine::embedAsync(const std::string& text, const std::string& id,
                               EmbeddingCallback callback) {
    std::thread([this, text, id, callback = std::move(callback)]() mutable {
        auto vector = embed(text);
        if (callback) {
            callback(id, vector);
        }
    }).detach();
}

EmbeddingEngine::Stats EmbeddingEngine::getStats() const {
    Stats stats;
    stats.totalEmbeddings = totalEmbeddings_.load();
    stats.batchCount = batchCount_.load();
    stats.totalLatencyMs = totalLatencyMs_.load();
    stats.avgLatencyMs = totalEmbeddings_.load() > 0 
        ? totalLatencyMs_.load() / totalEmbeddings_.load() 
        : 0.0;
    return stats;
}

void EmbeddingEngine::resetStats() {
    totalEmbeddings_ = 0;
    batchCount_ = 0;
    totalLatencyMs_ = 0;
}

std::vector<float> EmbeddingEngine::preprocessText(const std::string& text) {
    std::vector<float> tokens;
    tokens.reserve(text.size());

    for (unsigned char c : text) {
        if (c < 128) {
            tokens.push_back(static_cast<float>(c) / 127.0f);
        }
    }

    return tokens;
}

std::vector<float> EmbeddingEngine::runInference(const std::vector<int64_t>& tokens) {
    std::vector<float> output(config_.embeddingDim, 0.0f);

    for (size_t i = 0; i < output.size() && i < tokens.size(); ++i) {
        output[i] = static_cast<float>(tokens[i] % 1000) / 500.0f - 1.0f;
    }

    return output;
}

// ============================================================================
// EmbeddingStore
// ============================================================================

EmbeddingStore::EmbeddingStore()
    : connected_(false) {
    engine_ = std::make_shared<EmbeddingEngine>();
}

void EmbeddingStore::setEngine(std::shared_ptr<EmbeddingEngine> engine) {
    if (engine) {
        engine_ = std::move(engine);
    }
}

EmbeddingStore::~EmbeddingStore() {
    shutdown();
}

bool EmbeddingStore::initialize(const std::string& qdrantUrl, const std::string& dbPath) {
    LOG_INFO("EmbeddingStore initializing...");
    LOG_INFO("  Qdrant: {}", qdrantUrl);
    LOG_INFO("  SQLite DB: {}", dbPath);

    EmbeddingConfig config;
    if (!engine_->initialize(config)) {
        LOG_ERROR("Failed to initialize embedding engine");
        return false;
    }

    connected_ = true;
    LOG_INFO("EmbeddingStore initialized");
    return true;
}

void EmbeddingStore::shutdown() {
    if (!connected_.exchange(false)) {
        return;
    }

    engine_->shutdown();
    LOG_INFO("EmbeddingStore shutdown complete");
}

bool EmbeddingStore::add(const EmbeddingResult& embedding) {
    if (!connected_) {
        LOG_ERROR("EmbeddingStore not connected");
        return false;
    }

    std::lock_guard<std::mutex> lock(vectorsMutex_);

    if (vectors_.contains(embedding.id)) {
        LOG_DEBUG("Embedding {} already exists", embedding.id);
        return false;
    }

    if (seenHashes_.contains(embedding.contentHash)) {
        LOG_DEBUG("Embedding with hash {} already indexed", embedding.contentHash);
        return false;
    }

    vectors_[embedding.id] = embedding;
    targetIndex_[embedding.targetId].push_back(embedding.id);
    seenHashes_.insert(embedding.contentHash);
    totalVectors_++;

    LOG_DEBUG("Added embedding {} for target {}", embedding.id, embedding.targetId.value);
    return true;
}

bool EmbeddingStore::addBatch(const std::vector<EmbeddingResult>& embeddings) {
    size_t added = 0;
    for (const auto& emb : embeddings) {
        if (add(emb)) added++;
    }
    LOG_INFO("Batch added {} embeddings", added);
    return added > 0;
}

bool EmbeddingStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(vectorsMutex_);

    auto it = vectors_.find(id);
    if (it == vectors_.end()) {
        return false;
    }

    const auto& emb = it->second;
    seenHashes_.erase(emb.contentHash);

    auto& targetIds = targetIndex_[emb.targetId];
    targetIds.erase(
        std::remove(targetIds.begin(), targetIds.end(), id),
        targetIds.end()
    );

    vectors_.erase(it);
    totalVectors_--;

    return true;
}

bool EmbeddingStore::removeByTarget(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(vectorsMutex_);

    auto it = targetIndex_.find(targetId);
    if (it == targetIndex_.end()) {
        return false;
    }

    for (const auto& id : it->second) {
        auto vecIt = vectors_.find(id);
        if (vecIt != vectors_.end()) {
            seenHashes_.erase(vecIt->second.contentHash);
            vectors_.erase(vecIt);
        }
    }

    targetIndex_.erase(it);
    LOG_INFO("Removed all embeddings for target {}", targetId.value);
    return true;
}

std::vector<EmbeddingResult> EmbeddingStore::search(const std::string& query, size_t topK) {
    if (!connected_) {
        return {};
    }

    std::vector<float> queryVector = engine_->embed(query);
    return searchWithVector(queryVector, topK);
}

std::vector<EmbeddingResult> EmbeddingStore::search(const std::string& query, 
                                                 const TargetId& targetId, size_t topK) {
    if (!connected_) {
        return {};
    }

    std::vector<float> queryVector = engine_->embed(query);
    return searchWithVectorFiltered(queryVector, targetId, topK);
}

std::vector<EmbeddingResult> EmbeddingStore::search(const std::string& query,
                                                  const TargetId& targetId,
                                                  uint64_t timeStart, uint64_t timeEnd,
                                                  size_t topK) {
    if (!connected_) {
        return {};
    }

    std::vector<float> queryVector = engine_->embed(query);
    return searchWithVectorFilteredTime(queryVector, targetId, timeStart, timeEnd, topK);
}

std::vector<EmbeddingResult> EmbeddingStore::searchWithVector(
    const std::vector<float>& queryVector, size_t topK) {

    std::lock_guard<std::mutex> lock(vectorsMutex_);

    std::vector<std::pair<const EmbeddingResult*, float>> scored;
    scored.reserve(vectors_.size());

    for (const auto& [id, emb] : vectors_) {
        if (emb.vector.empty()) continue;
        float score = cosineSimilarity(queryVector, emb.vector);
        scored.push_back({&emb, score});
    }

    std::partial_sort(
        scored.begin(),
        scored.begin() + std::min(topK, scored.size()),
        scored.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; }
    );

    std::vector<EmbeddingResult> results;
    for (size_t i = 0; i < std::min(topK, scored.size()); ++i) {
        results.push_back(*scored[i].first);
    }

    return results;
}

std::vector<EmbeddingResult> EmbeddingStore::searchWithVectorFiltered(
    const std::vector<float>& queryVector, const TargetId& targetId, size_t topK) {

    std::lock_guard<std::mutex> lock(vectorsMutex_);

    auto targetIt = targetIndex_.find(targetId);
    if (targetIt == targetIndex_.end()) {
        return {};
    }

    std::vector<std::pair<const EmbeddingResult*, float>> scored;
    scored.reserve(targetIt->second.size());

    for (const auto& id : targetIt->second) {
        auto vecIt = vectors_.find(id);
        if (vecIt != vectors_.end() && !vecIt->second.vector.empty()) {
            float score = cosineSimilarity(queryVector, vecIt->second.vector);
            scored.push_back({&vecIt->second, score});
        }
    }

    std::partial_sort(
        scored.begin(),
        scored.begin() + std::min(topK, scored.size()),
        scored.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; }
    );

    std::vector<EmbeddingResult> results;
    for (size_t i = 0; i < std::min(topK, scored.size()); ++i) {
        results.push_back(*scored[i].first);
    }

    return results;
}

std::vector<EmbeddingResult> EmbeddingStore::searchWithVectorFilteredTime(
    const std::vector<float>& queryVector, const TargetId& targetId,
    uint64_t timeStart, uint64_t timeEnd, size_t topK) {

    std::lock_guard<std::mutex> lock(vectorsMutex_);

    auto targetIt = targetIndex_.find(targetId);
    if (targetIt == targetIndex_.end()) {
        return {};
    }

    std::vector<std::pair<const EmbeddingResult*, float>> scored;
    scored.reserve(targetIt->second.size());

    for (const auto& id : targetIt->second) {
        auto vecIt = vectors_.find(id);
        if (vecIt != vectors_.end() && !vecIt->second.vector.empty()) {
            if (vecIt->second.timestamp >= timeStart && vecIt->second.timestamp <= timeEnd) {
                float score = cosineSimilarity(queryVector, vecIt->second.vector);
                scored.push_back({&vecIt->second, score});
            }
        }
    }

    std::partial_sort(
        scored.begin(),
        scored.begin() + std::min(topK, scored.size()),
        scored.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; }
    );

    std::vector<EmbeddingResult> results;
    for (size_t i = 0; i < std::min(topK, scored.size()); ++i) {
        results.push_back(*scored[i].first);
    }

    return results;
}

std::optional<EmbeddingResult> EmbeddingStore::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(vectorsMutex_);

    auto it = vectors_.find(id);
    if (it != vectors_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<EmbeddingResult> EmbeddingStore::getByTarget(const TargetId& targetId, size_t limit) const {
    std::lock_guard<std::mutex> lock(vectorsMutex_);

    std::vector<EmbeddingResult> results;
    auto it = targetIndex_.find(targetId);

    if (it != targetIndex_.end()) {
        for (auto rit = it->second.rbegin(); 
             rit != it->second.rend() && results.size() < limit; ++rit) {
            auto vecIt = vectors_.find(*rit);
            if (vecIt != vectors_.end()) {
                results.push_back(vecIt->second);
            }
        }
    }

    return results;
}

bool EmbeddingStore::exists(uint64_t contentHash) const {
    std::lock_guard<std::mutex> lock(vectorsMutex_);
    return seenHashes_.contains(contentHash);
}

size_t EmbeddingStore::count() const {
    std::lock_guard<std::mutex> lock(vectorsMutex_);
    return vectors_.size();
}

size_t EmbeddingStore::countByTarget(const TargetId& targetId) const {
    std::lock_guard<std::mutex> lock(vectorsMutex_);
    auto it = targetIndex_.find(targetId);
    return it != targetIndex_.end() ? it->second.size() : 0;
}

void EmbeddingStore::clear() {
    std::lock_guard<std::mutex> lock(vectorsMutex_);
    vectors_.clear();
    targetIndex_.clear();
    seenHashes_.clear();
    totalVectors_ = 0;
    LOG_INFO("EmbeddingStore cleared");
}

void EmbeddingStore::vacuum() {
    LOG_INFO("EmbeddingStore vacuum (no-op for in-memory store)");
}

float EmbeddingStore::cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0f;
    }

    float dot = 0.0f;
    float normA = 0.0f;
    float normB = 0.0f;

    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }

    float denominator = std::sqrt(normA) * std::sqrt(normB);
    return denominator > 0.0f ? dot / denominator : 0.0f;
}

EmbeddingStore::Stats EmbeddingStore::getStats() const {
    Stats stats;
    stats.totalVectors = totalVectors_.load();
    stats.indexedTargets = targetIndex_.size();
    return stats;
}

// ============================================================================
// IncrementalEmbeddingPipeline
// ============================================================================

IncrementalEmbeddingPipeline::IncrementalEmbeddingPipeline()
    : textQueue_(1000)
    , running_(false) {
}

IncrementalEmbeddingPipeline::~IncrementalEmbeddingPipeline() {
    stop();
}

void IncrementalEmbeddingPipeline::setEmbeddingEngine(std::shared_ptr<EmbeddingEngine> engine) {
    engine_ = std::move(engine);
}

void IncrementalEmbeddingPipeline::setEmbeddingStore(std::shared_ptr<EmbeddingStore> store) {
    store_ = std::move(store);
}

void IncrementalEmbeddingPipeline::start() {
    if (running_.exchange(true)) return;

    if (store_) {
        store_->initialize();
    }

    processingThread_ = std::jthread([this](std::stop_token token) {
        while (!token.stop_requested()) {
            processQueue();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    LOG_INFO("IncrementalEmbeddingPipeline started");
}

void IncrementalEmbeddingPipeline::stop() {
    if (!running_.exchange(false)) return;

    if (processingThread_.joinable()) {
        processingThread_.request_stop();
        processingThread_.join();
    }

    LOG_INFO("IncrementalEmbeddingPipeline stopped");
}

void IncrementalEmbeddingPipeline::addText(const std::string& text, 
                                         const TemporalObservation& obs) {
    if (!running_) return;

    textQueue_.push({text, obs});
}

void IncrementalEmbeddingPipeline::addChunk(const TextChunk& chunk) {
    if (!running_) return;

    TemporalObservation obs;
    obs.targetId = chunk.targetId;
    obs.url = chunk.url.value;
    obs.timestamp = chunk.timestamp;
    obs.source = chunk.source;
    obs.contentHash = std::hash<std::string>{}(chunk.text);

    textQueue_.push({chunk.text, obs});
}

void IncrementalEmbeddingPipeline::processNow() {
    processQueue();
}

size_t IncrementalEmbeddingPipeline::pendingCount() const {
    return textQueue_.size();
}

EmbeddingEngine::Stats IncrementalEmbeddingPipeline::getEmbeddingStats() const {
    if (engine_) {
        return engine_->getStats();
    }
    return {};
}

EmbeddingStore::Stats IncrementalEmbeddingPipeline::getStoreStats() const {
    if (store_) {
        return store_->getStats();
    }
    return {};
}

void IncrementalEmbeddingPipeline::processQueue() {
    while (running_) {
        auto item = textQueue_.popFor(std::chrono::milliseconds(10));
        if (!item) break;

        const auto& [text, obs] = *item;

        if (store_ && store_->exists(obs.contentHash)) {
            duplicatesSkipped_++;
            LOG_DEBUG("Skipping duplicate embedding: hash={}", obs.contentHash);
            continue;
        }

        if (!engine_) {
            LOG_ERROR("No embedding engine configured");
            continue;
        }

        std::vector<float> vector = engine_->embed(text);

        EmbeddingResult emb;
        // Unique id: timestamp + atomic sequence (safe under concurrent drain
        // by both the background worker and processNow()).
        emb.id = "emb_" + std::to_string(obs.timestamp) + "_" +
                 std::to_string(idSeq_.fetch_add(1) + 1);
        emb.targetId = obs.targetId;
        emb.content = text;
        emb.vector = std::move(vector);
        emb.timestamp = obs.timestamp;
        emb.url = PageUrl(obs.url);
        emb.source = obs.source;
        emb.confidence = obs.confidence;
        emb.contentHash = obs.contentHash;

        if (store_) {
            store_->add(emb);
            vectorsGenerated_++;
        }

        textsProcessed_++;
    }
}

} // namespace edgemon
