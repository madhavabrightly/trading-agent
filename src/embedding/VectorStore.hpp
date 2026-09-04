#pragma once

#include "common.hpp"
#include "embedding/EmbeddingEngine.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace edgemon {

struct VectorRecord {
    std::string id;
    std::string targetId;
    std::string content;
    std::vector<float> embedding;
    std::string url;
    TimePoint timestamp;
    int64_t timestampEpoch;
};

struct SearchResult {
    std::string id;
    std::string content;
    float score;
    TimePoint timestamp;
};

class VectorStore {
public:
    explicit VectorStore(std::shared_ptr<EmbeddingEngine> engine);
    ~VectorStore();

    bool initialize(const std::string& dbPath = "");
    bool isConnected() const { return connected_; }

    bool add(const VectorRecord& record);
    bool addBatch(const std::vector<VectorRecord>& records);
    
    bool remove(const std::string& id);
    bool removeByTarget(const std::string& targetId);
    
    bool update(const VectorRecord& record);

    std::vector<SearchResult> search(const std::string& query, size_t topK = 10);
    std::vector<SearchResult> searchByTarget(const std::string& targetId, size_t topK = 10);
    std::vector<SearchResult> searchByTimeRange(TimePoint start, TimePoint end, size_t topK = 10);

    std::optional<VectorRecord> get(const std::string& id) const;
    std::vector<VectorRecord> getByTarget(const std::string& targetId) const;

    size_t count() const { return records_.size(); }
    size_t countByTarget(const std::string& targetId) const;

    void clear();

    // Persistence: vectors survive across CLI invocations (JSON snapshot).
    void setPersistencePath(const std::string& path) { persistencePath_ = path; }
    void saveToDisk() const;
    void loadFromDisk();

private:
    float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    std::vector<SearchResult> internalSearch(const std::vector<float>& queryEmbedding, 
                                              size_t topK,
                                              std::function<bool(const VectorRecord&)> filter);

    std::shared_ptr<EmbeddingEngine> engine_;
    std::atomic<bool> connected_;
    
    mutable std::mutex recordsMutex_;
    std::unordered_map<std::string, VectorRecord> records_;
    std::unordered_map<std::string, std::vector<std::string>> targetIndex_;

    std::string persistencePath_ = "vectors.json";
};

}
