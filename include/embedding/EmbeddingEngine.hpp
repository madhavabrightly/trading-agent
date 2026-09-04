#pragma once

#include "core/Types.hpp"
#include "extraction/TextProcessor.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>

namespace edgemon {

struct EmbeddingResult {
    std::string id;
    TargetId targetId;
    std::string content;
    std::vector<float> vector;
    uint64_t timestamp = 0;
    PageUrl url;
    std::string title;
    ContentSource source = ContentSource::Unknown;
    float confidence = 0.0f;
    uint64_t contentHash = 0;
};

struct EmbeddingConfig {
    std::string modelPath;
    std::string modelName = "BGE-small-en-v1.5";
    size_t embeddingDim = 768;
    size_t batchSize = 32;
    size_t maxSequenceLength = 512;
    bool normalize = true;
    std::string poolingType = "mean";
};

class EmbeddingEngine {
public:
    using EmbeddingCallback = std::function<void(const std::string& id, const std::vector<float>& vector)>;

    EmbeddingEngine();
    ~EmbeddingEngine();

    bool initialize(const EmbeddingConfig& config = EmbeddingConfig());
    void shutdown();
    bool isInitialized() const { return initialized_.load(); }

    std::vector<float> embed(const std::string& text);
    std::vector<float> embedSync(const std::string& text);
    
    void embedBatch(const std::vector<std::string>& texts, 
                   std::vector<std::vector<float>>& results);
    
    void embedAsync(const std::string& text, const std::string& id, 
                   EmbeddingCallback callback);

    size_t getDimension() const { return config_.embeddingDim; }
    std::string getModelName() const { return config_.modelName; }
    EmbeddingConfig getConfig() const { return config_; }

    struct Stats {
        size_t totalEmbeddings = 0;
        size_t batchCount = 0;
        double avgLatencyMs = 0;
        double totalLatencyMs = 0;
    };
    Stats getStats() const;
    void resetStats();

private:
    std::vector<float> preprocessText(const std::string& text);
    std::vector<float> runInference(const std::vector<int64_t>& tokens);

    EmbeddingConfig config_;
    std::atomic<bool> initialized_;
    
    void* onnxSession_ = nullptr;
    void* tokenizer_ = nullptr;
    
    std::atomic<size_t> totalEmbeddings_{0};
    std::atomic<size_t> batchCount_{0};
    std::atomic<double> totalLatencyMs_{0};
};

class EmbeddingStore {
public:
    EmbeddingStore();
    ~EmbeddingStore();

    // Use an externally owned engine so stored vectors and query vectors are
    // produced by the same model/dimension (critical for cosine search).
    void setEngine(std::shared_ptr<EmbeddingEngine> engine);

    bool initialize(const std::string& qdrantUrl = "localhost:6334",
                   const std::string& dbPath = "embeddings.db");
    void shutdown();

    bool add(const EmbeddingResult& embedding);
    bool addBatch(const std::vector<EmbeddingResult>& embeddings);
    
    bool remove(const std::string& id);
    bool removeByTarget(const TargetId& targetId);
    
    std::vector<EmbeddingResult> search(const std::string& query, size_t topK = 10);
    std::vector<EmbeddingResult> search(const std::string& query, const TargetId& targetId, size_t topK = 10);
    std::vector<EmbeddingResult> search(const std::string& query, 
                                       const TargetId& targetId,
                                       uint64_t timeStart, uint64_t timeEnd,
                                       size_t topK = 10);

    std::optional<EmbeddingResult> get(const std::string& id) const;
    std::vector<EmbeddingResult> getByTarget(const TargetId& targetId, size_t limit = 100) const;
    
    bool exists(uint64_t contentHash) const;
    
    size_t count() const;
    size_t countByTarget(const TargetId& targetId) const;

    void clear();
    void vacuum();

    struct Stats {
        size_t totalVectors = 0;
        size_t indexedTargets = 0;
        size_t lastSearchMs = 0;
    };
    Stats getStats() const;

private:
    std::vector<EmbeddingResult> localSearch(const std::string& query, size_t topK);
    std::vector<EmbeddingResult> searchWithVector(const std::vector<float>& queryVector, size_t topK);
    std::vector<EmbeddingResult> searchWithVectorFiltered(const std::vector<float>& queryVector, 
                                                        const TargetId& targetId, size_t topK);
    std::vector<EmbeddingResult> searchWithVectorFilteredTime(const std::vector<float>& queryVector,
                                                            const TargetId& targetId,
                                                            uint64_t timeStart, uint64_t timeEnd,
                                                            size_t topK);
    float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    
    std::shared_ptr<EmbeddingEngine> engine_;
    std::atomic<bool> connected_;
    
    mutable std::mutex vectorsMutex_;
    std::unordered_map<std::string, EmbeddingResult> vectors_;
    std::unordered_map<TargetId, std::vector<std::string>, std::hash<TargetId>> targetIndex_;
    std::unordered_set<uint64_t> seenHashes_;
    
    std::atomic<size_t> totalVectors_{0};
};

class IncrementalEmbeddingPipeline {
public:
    IncrementalEmbeddingPipeline();
    ~IncrementalEmbeddingPipeline();

    void setEmbeddingEngine(std::shared_ptr<EmbeddingEngine> engine);
    void setEmbeddingStore(std::shared_ptr<EmbeddingStore> store);
    
    void setMaxQueueSize(size_t size) { maxQueueSize_ = size; }

    void start();
    void stop();

    void addText(const std::string& text, const TemporalObservation& obs);
    void addChunk(const TextChunk& chunk);
    
    void processNow();

    size_t pendingCount() const;
    
    EmbeddingEngine::Stats getEmbeddingStats() const;
    EmbeddingStore::Stats getStoreStats() const;

private:
    void processQueue();

    std::shared_ptr<EmbeddingEngine> engine_;
    std::shared_ptr<EmbeddingStore> store_;
    
    BlockingQueue<std::pair<std::string, TemporalObservation>> textQueue_;
    size_t maxQueueSize_ = 1000;
    
    std::atomic<bool> running_;
    std::jthread processingThread_;
    
    std::atomic<size_t> textsProcessed_{0};
    std::atomic<size_t> vectorsGenerated_{0};
    std::atomic<size_t> duplicatesSkipped_{0};
    std::atomic<uint64_t> idSeq_{0};
};

} // namespace edgemon
