#pragma once

#include "core/Types.hpp"
#include "extraction/OCRManager.hpp"
#include "extraction/TextNormalizer.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>

namespace edgemon {

struct TextChunk {
    std::string id;
    std::string text;
    std::string section;
    std::string heading;
    TargetId targetId;
    PageUrl url;
    uint64_t timestamp = 0;
    ContentSource source = ContentSource::Unknown;
    float confidence = 0.0f;
    size_t charCount = 0;
    size_t wordCount = 0;
};

struct TemporalObservation {
    std::string id;
    TargetId targetId;
    uint64_t timestamp = 0;
    ContentSource source = ContentSource::Unknown;
    
    std::string rawText;
    std::string normalizedText;
    std::string structuredText;
    
    float confidence = 0.0f;
    uint64_t contentHash = 0;
    uint64_t normalizedHash = 0;
    
    std::string sourceUrl;
    std::string url;
    std::vector<TextChunk> chunks;
    
    bool isDuplicate = false;
    bool isIndexed = false;
};

struct TextProcessingConfig {
    bool preserveNumbers = true;
    bool preserveTimestamps = true;
    bool preserveTables = true;
    bool removeOcrArtifacts = true;
    bool normalizeUnicode = true;
    bool collapseWhitespace = true;
    size_t maxChunkSize = 512;
    size_t minChunkSize = 50;
    float similarityThreshold = 0.95f;
};

class TextChunker {
public:
    TextChunker() = default;
    explicit TextChunker(const TextProcessingConfig& config);
    ~TextChunker() = default;

    void setConfig(const TextProcessingConfig& config);
    
    std::vector<TextChunk> chunk(const std::string& text, const TargetId& targetId,
                                const PageUrl& url, ContentSource source);

private:
    std::vector<TextChunk> chunkByParagraphs(const std::string& text, const TargetId& targetId,
                                             const PageUrl& url, ContentSource source);
    std::vector<TextChunk> chunkByLines(const std::string& text, const TargetId& targetId,
                                        const PageUrl& url, ContentSource source);
    std::vector<TextChunk> chunkBySize(const std::string& text, const TargetId& targetId,
                                       const PageUrl& url, ContentSource source);

    TextProcessingConfig config_;
};

class Deduplicator {
public:
    Deduplicator() = default;
    ~Deduplicator() = default;

    void setHashFunction(std::function<uint64_t(const std::string&)> func);
    
    bool isDuplicate(const std::string& text);
    bool isDuplicate(const std::string& text, uint64_t hash);
    
    void markSeen(const std::string& text, uint64_t hash);
    void markSeen(const TemporalObservation& obs);
    
    void forget(uint64_t hash);
    void forgetOlderThan(uint64_t timestamp);
    void clear();

    size_t seenCount() const;
    size_t recentCount() const;

    struct Stats {
        size_t totalChecked = 0;
        size_t duplicates = 0;
        size_t unique = 0;
    };
    Stats getStats() const;

private:
    std::unordered_set<uint64_t> seenHashes_;
    std::unordered_map<uint64_t, uint64_t> hashTimestamps_;
    std::function<uint64_t(const std::string&)> hashFunc_;
    
    std::atomic<size_t> totalChecked_{0};
    std::atomic<size_t> duplicates_{0};
    std::atomic<size_t> unique_{0};
    
    mutable std::mutex mutex_;
};

class TemporalStore {
public:
    TemporalStore();
    ~TemporalStore();

    std::string add(const TemporalObservation& observation);
    std::string add(const OCRResult& result, const TargetId& targetId, ContentSource source);
    
    std::optional<TemporalObservation> get(const std::string& id) const;
    std::vector<TemporalObservation> getByTarget(const TargetId& targetId, size_t limit = 100) const;
    std::vector<TemporalObservation> getByTimeRange(uint64_t start, uint64_t end, size_t limit = 100) const;
    std::vector<TemporalObservation> getRecent(size_t limit = 100) const;
    
    bool remove(const std::string& id);
    bool removeByTarget(const TargetId& targetId);
    bool removeOlderThan(uint64_t timestamp);
    
    bool markIndexed(const std::string& id);
    bool markIndexed(const std::string& id, const std::vector<TextChunk>& chunks);
    
    size_t count() const;
    size_t countByTarget(const TargetId& targetId) const;
    size_t countIndexed() const;
    size_t countUnindexed() const;
    
    void clear();
    
    struct Stats {
        size_t total = 0;
        size_t indexed = 0;
        size_t unindexed = 0;
        size_t duplicates = 0;
        uint64_t oldest = 0;
        uint64_t newest = 0;
    };
    Stats getStats() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TemporalObservation> observations_;
    std::unordered_map<TargetId, std::vector<std::string>, std::hash<TargetId>> targetIndex_;
    std::vector<std::string> idOrder_;
    
    size_t maxStored_ = 100000;
    uint64_t lastCleanup_ = 0;
    size_t cleanupInterval_ = 1000;
};

class TextProcessingPipeline {
public:
    TextProcessingPipeline();
    ~TextProcessingPipeline();

    void setNormalizerConfig(const TextProcessingConfig& config);
    void setChunkerConfig(const TextProcessingConfig& config);
    
    void addObserver(std::function<void(const TemporalObservation&)> observer);
    void addChunkObserver(std::function<void(const TextChunk&)> observer);
    
    TemporalObservation process(const std::string& rawText, const TargetId& targetId,
                             const PageUrl& url, ContentSource source);
    
    TemporalObservation process(const OCRResult& ocrResult, const TargetId& targetId);
    TemporalObservation process(const std::string& rawText, ContentSource source);
    
    void setDeduplicationEnabled(bool enabled);
    void setIndexingEnabled(bool enabled);

    void flush();
    
    TemporalStore& getStore() { return store_; }
    const TemporalStore& getStore() const { return store_; }
    
    size_t pendingCount() const;

private:
    TextNormalizer normalizer_;
    TextChunker chunker_;
    Deduplicator deduplicator_;
    TemporalStore store_;
    
    std::vector<std::function<void(const TemporalObservation&)>> observers_;
    std::vector<std::function<void(const TextChunk&)>> chunkObservers_;
    
    std::atomic<bool> deduplicationEnabled_{true};
    std::atomic<bool> indexingEnabled_{true};
    
    mutable std::mutex observerMutex_;
};

} // namespace edgemon
