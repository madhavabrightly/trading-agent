#include "embedding/VectorStore.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>

namespace edgemon {

VectorStore::VectorStore(std::shared_ptr<EmbeddingEngine> engine)
    : engine_(std::move(engine))
    , connected_(false) {
}

VectorStore::~VectorStore() {
    saveToDisk();
}

bool VectorStore::initialize(const std::string& dbPath) {
    LOG_INFO("VectorStore initializing{}...", dbPath.empty() ? "" : ": " + dbPath);
    
    if (!dbPath.empty()) {
        persistencePath_ = dbPath;
    }
    loadFromDisk();
    
    connected_ = true;
    LOG_INFO("VectorStore initialized with {} records", records_.size());
    return true;
}

bool VectorStore::add(const VectorRecord& record) {
    if (!connected_) {
        LOG_ERROR("VectorStore not connected");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        
        if (records_.contains(record.id)) {
            LOG_WARN("Record {} already exists", record.id);
            return false;
        }

        records_[record.id] = record;
        targetIndex_[record.targetId].push_back(record.id);
    }
    
    saveToDisk();
    LOG_DEBUG("Added record {} to target {}", record.id, record.targetId);
    return true;
}

bool VectorStore::addBatch(const std::vector<VectorRecord>& records) {
    if (!connected_) {
        LOG_ERROR("VectorStore not connected");
        return false;
    }

    std::lock_guard<std::mutex> lock(recordsMutex_);
    
    size_t added = 0;
    for (const auto& record : records) {
        if (!records_.contains(record.id)) {
            records_[record.id] = record;
            targetIndex_[record.targetId].push_back(record.id);
            added++;
        }
    }
    
    LOG_INFO("Batch added {} records", added);
    return true;
}

bool VectorStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    
    auto it = records_.find(id);
    if (it == records_.end()) {
        return false;
    }

    const auto& record = it->second;
    auto& targetIds = targetIndex_[record.targetId];
    targetIds.erase(
        std::remove(targetIds.begin(), targetIds.end(), id),
        targetIds.end()
    );

    records_.erase(it);
    LOG_DEBUG("Removed record {}", id);
    return true;
}

bool VectorStore::removeByTarget(const std::string& targetId) {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    
    auto targetIt = targetIndex_.find(targetId);
    if (targetIt == targetIndex_.end()) {
        return false;
    }

    for (const auto& id : targetIt->second) {
        records_.erase(id);
    }
    targetIndex_.erase(targetIt);
    
    LOG_INFO("Removed all records for target {}", targetId);
    return true;
}

bool VectorStore::update(const VectorRecord& record) {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    
    if (!records_.contains(record.id)) {
        return false;
    }

    records_[record.id] = record;
    LOG_DEBUG("Updated record {}", record.id);
    return true;
}

std::vector<SearchResult> VectorStore::search(const std::string& query, size_t topK) {
    if (!connected_ || !engine_) {
        LOG_ERROR("VectorStore not ready for search");
        return {};
    }

    auto queryEmbedding = engine_->embed(query);
    return internalSearch(queryEmbedding, topK, [](const VectorRecord&) { return true; });
}

std::vector<SearchResult> VectorStore::searchByTarget(const std::string& targetId, size_t topK) {
    if (!connected_ || !engine_) {
        return {};
    }

    auto targets = getByTarget(targetId);
    if (targets.empty()) {
        return {};
    }

    std::string combined;
    for (const auto& t : targets) {
        combined += t.content + " ";
    }
    
    auto queryEmbedding = engine_->embed(combined);
    
    auto filter = [&targetId](const VectorRecord& r) {
        return r.targetId == targetId;
    };
    
    return internalSearch(queryEmbedding, topK, filter);
}

std::vector<SearchResult> VectorStore::searchByTimeRange(TimePoint start, TimePoint end, size_t topK) {
    if (!connected_) {
        return {};
    }

    auto filter = [&start, &end](const VectorRecord& r) {
        return r.timestamp >= start && r.timestamp <= end;
    };

    std::vector<float> dummyEmbedding(records_.begin()->second.embedding.size(), 0.0f);
    return internalSearch(dummyEmbedding, topK, filter);
}

std::optional<VectorRecord> VectorStore::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    
    auto it = records_.find(id);
    if (it != records_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<VectorRecord> VectorStore::getByTarget(const std::string& targetId) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    
    std::vector<VectorRecord> results;
    auto it = targetIndex_.find(targetId);
    
    if (it != targetIndex_.end()) {
        results.reserve(it->second.size());
        for (const auto& id : it->second) {
            if (auto recIt = records_.find(id); recIt != records_.end()) {
                results.push_back(recIt->second);
            }
        }
    }
    
    return results;
}

size_t VectorStore::countByTarget(const std::string& targetId) const {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    auto it = targetIndex_.find(targetId);
    return it != targetIndex_.end() ? it->second.size() : 0;
}

void VectorStore::clear() {
    std::lock_guard<std::mutex> lock(recordsMutex_);
    records_.clear();
    targetIndex_.clear();
    LOG_INFO("VectorStore cleared");
}

float VectorStore::cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
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

std::vector<SearchResult> VectorStore::internalSearch(
    const std::vector<float>& queryEmbedding,
    size_t topK,
    std::function<bool(const VectorRecord&)> filter) {
    
    std::vector<std::pair<const VectorRecord*, float>> scored;
    
    {
        std::lock_guard<std::mutex> lock(recordsMutex_);
        for (const auto& [id, record] : records_) {
            if (filter(record) && !record.embedding.empty()) {
                float score = cosineSimilarity(queryEmbedding, record.embedding);
                scored.push_back({&record, score});
            }
        }
    }
    
    std::partial_sort(
        scored.begin(), 
        scored.begin() + std::min(topK, scored.size()),
        scored.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; }
    );
    
    std::vector<SearchResult> results;
    for (size_t i = 0; i < std::min(topK, scored.size()); ++i) {
        SearchResult r;
        r.id = scored[i].first->id;
        r.content = scored[i].first->content;
        r.score = scored[i].second;
        r.timestamp = scored[i].first->timestamp;
        results.push_back(r);
    }
    
    return results;
}

void VectorStore::saveToDisk() const {
    if (persistencePath_.empty()) return;

    try {
        std::lock_guard<std::mutex> lock(recordsMutex_);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& [id, rec] : records_) {
            nlohmann::json j;
            j["id"] = rec.id;
            j["targetId"] = rec.targetId;
            j["content"] = rec.content;
            j["url"] = rec.url;
            j["timestampEpoch"] = rec.timestampEpoch;
            j["embedding"] = rec.embedding;
            arr.push_back(std::move(j));
        }

        std::ofstream out(persistencePath_, std::ios::trunc);
        if (out) {
            out << arr.dump();
        }
    } catch (const std::exception& e) {
        LOG_WARN("VectorStore save failed: {}", e.what());
    }
}

void VectorStore::loadFromDisk() {
    if (persistencePath_.empty()) return;

    try {
        std::ifstream in(persistencePath_);
        if (!in) return;

        nlohmann::json arr = nlohmann::json::parse(in);
        if (!arr.is_array()) return;

        std::lock_guard<std::mutex> lock(recordsMutex_);
        records_.clear();
        targetIndex_.clear();

        for (const auto& j : arr) {
            VectorRecord rec;
            rec.id = j.value("id", std::string());
            rec.targetId = j.value("targetId", std::string());
            rec.content = j.value("content", std::string());
            rec.url = j.value("url", std::string());
            rec.timestampEpoch = j.value("timestampEpoch", int64_t(0));
            rec.timestamp = TimePoint(std::chrono::seconds(rec.timestampEpoch));
            if (j.contains("embedding") && j["embedding"].is_array()) {
                rec.embedding = j["embedding"].get<std::vector<float>>();
            }
            if (rec.id.empty()) continue;
            records_[rec.id] = std::move(rec);
            targetIndex_[rec.targetId].push_back(rec.id);
        }
        LOG_INFO("VectorStore loaded {} records from {}", records_.size(), persistencePath_);
    } catch (const std::exception& e) {
        LOG_WARN("VectorStore load failed: {}", e.what());
    }
}

}
