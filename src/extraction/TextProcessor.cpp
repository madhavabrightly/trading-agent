#include "extraction/TextProcessor.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace edgemon {

// Stable uint64 folding of the canonical TextNormalizer string hash, for
// deduplication + content-hash tracking (canonical hash is a hex string).
static uint64_t hashToUint64(const std::string& hash) {
    uint64_t out = 5381;
    for (unsigned char c : hash) {
        out = ((out << 5) + out) + c;
    }
    return out;
}

// ============================================================================
// TextChunker
// ============================================================================

TextChunker::TextChunker(const TextProcessingConfig& config) : config_(config) {}

void TextChunker::setConfig(const TextProcessingConfig& config) {
    config_ = config;
}

std::vector<TextChunk> TextChunker::chunk(const std::string& text, const TargetId& targetId,
                                           const PageUrl& url, ContentSource source) {
    if (text.empty()) return {};

    auto chunks = chunkByParagraphs(text, targetId, url, source);
    if (chunks.size() > 1) return chunks;

    chunks = chunkByLines(text, targetId, url, source);
    if (chunks.size() > 1) return chunks;

    return chunkBySize(text, targetId, url, source);
}

std::vector<TextChunk> TextChunker::chunkByParagraphs(const std::string& text, const TargetId& targetId,
                                                       const PageUrl& url, ContentSource source) {
    std::vector<TextChunk> chunks;

    std::regex paraPattern(R"(\n\s*\n)");
    std::sregex_token_iterator it(text.begin(), text.end(), paraPattern, -1);
    std::sregex_token_iterator end;

    size_t index = 0;
    while (it != end) {
        std::string para = *it++;
        para = std::regex_replace(para, std::regex(R"(^\s+|\s+$)"), "");

        if (para.length() >= config_.minChunkSize) {
            TextChunk chunk;
            chunk.id = targetId.value + "_p" + std::to_string(index);
            chunk.text = para;
            chunk.targetId = targetId;
            chunk.url = url;
            chunk.source = source;
            chunk.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
            chunk.charCount = para.length();
            chunks.push_back(chunk);
        }
        index++;
    }

    return chunks;
}

std::vector<TextChunk> TextChunker::chunkByLines(const std::string& text, const TargetId& targetId,
                                                    const PageUrl& url, ContentSource source) {
    std::vector<TextChunk> chunks;

    std::istringstream stream(text);
    std::string line;
    size_t index = 0;

    while (std::getline(stream, line)) {
        line = std::regex_replace(line, std::regex(R"(^\s+|\s+$)"), "");

        if (line.length() >= config_.minChunkSize) {
            TextChunk chunk;
            chunk.id = targetId.value + "_l" + std::to_string(index);
            chunk.text = line;
            chunk.targetId = targetId;
            chunk.url = url;
            chunk.source = source;
            chunk.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
            chunk.charCount = line.length();
            chunks.push_back(chunk);
        }
        index++;
    }

    return chunks;
}

std::vector<TextChunk> TextChunker::chunkBySize(const std::string& text, const TargetId& targetId,
                                                  const PageUrl& url, ContentSource source) {
    std::vector<TextChunk> chunks;

    if (text.empty()) return chunks;

    size_t pos = 0;
    size_t index = 0;

    while (pos < text.size()) {
        size_t end = std::min(pos + config_.maxChunkSize, text.size());

        if (end < text.size()) {
            size_t breakPos = text.find(' ', end);
            if (breakPos != std::string::npos && breakPos > pos + config_.maxChunkSize / 2) {
                end = breakPos;
            }
        }

        std::string chunkText = text.substr(pos, end - pos);
        chunkText = std::regex_replace(chunkText, std::regex(R"(^\s+|\s+$)"), "");

        if (!chunkText.empty()) {
            TextChunk chunk;
            chunk.id = targetId.value + "_c" + std::to_string(index);
            chunk.text = chunkText;
            chunk.targetId = targetId;
            chunk.url = url;
            chunk.source = source;
            chunk.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
            chunk.charCount = chunkText.length();
            chunks.push_back(chunk);
        }

        pos = end;
        index++;
    }

    return chunks;
}

// ============================================================================
// Deduplicator
// ============================================================================

void Deduplicator::setHashFunction(std::function<uint64_t(const std::string&)> func) {
    hashFunc_ = std::move(func);
}

bool Deduplicator::isDuplicate(const std::string& text) {
    if (!hashFunc_) {
        TextNormalizer normalizer;
        uint64_t hash = hashToUint64(normalizer.computeHash(text));
        return isDuplicate(text, hash);
    }
    uint64_t hash = hashFunc_(text);
    return isDuplicate(text, hash);
}

bool Deduplicator::isDuplicate(const std::string& text, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    totalChecked_++;

    if (seenHashes_.count(hash)) {
        duplicates_++;
        return true;
    }

    unique_++;
    return false;
}

void Deduplicator::markSeen(const std::string& text, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    seenHashes_.insert(hash);
}

void Deduplicator::markSeen(const TemporalObservation& obs) {
    std::lock_guard<std::mutex> lock(mutex_);
    seenHashes_.insert(obs.contentHash);
    hashTimestamps_[obs.contentHash] = obs.timestamp;
}

void Deduplicator::forget(uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    seenHashes_.erase(hash);
    hashTimestamps_.erase(hash);
}

void Deduplicator::forgetOlderThan(uint64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint64_t> toRemove;
    for (const auto& [hash, ts] : hashTimestamps_) {
        if (ts < timestamp) {
            toRemove.push_back(hash);
        }
    }

    for (uint64_t hash : toRemove) {
        seenHashes_.erase(hash);
        hashTimestamps_.erase(hash);
    }
}

void Deduplicator::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    seenHashes_.clear();
    hashTimestamps_.clear();
    totalChecked_ = 0;
    duplicates_ = 0;
    unique_ = 0;
}

size_t Deduplicator::seenCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seenHashes_.size();
}

size_t Deduplicator::recentCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hashTimestamps_.size();
}

Deduplicator::Stats Deduplicator::getStats() const {
    Stats stats;
    stats.totalChecked = totalChecked_.load();
    stats.duplicates = duplicates_.load();
    stats.unique = unique_.load();
    return stats;
}

// ============================================================================
// TemporalStore
// ============================================================================

TemporalStore::TemporalStore() = default;

TemporalStore::~TemporalStore() = default;

std::string TemporalStore::add(const TemporalObservation& observation) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string id = observation.id.empty() 
        ? "obs_" + std::to_string(observations_.size())
        : observation.id;

    TemporalObservation obs = observation;
    obs.id = id;

    observations_[id] = obs;
    idOrder_.push_back(id);

    targetIndex_[obs.targetId].push_back(id);

    if (observations_.size() > maxStored_) {
        std::vector<std::string> toRemove;
        size_t removeCount = observations_.size() - maxStored_;
        for (size_t i = 0; i < removeCount && !idOrder_.empty(); ++i) {
            std::string oldId = idOrder_[i];
            if (observations_.contains(oldId)) {
                toRemove.push_back(oldId);
            }
        }

        for (const auto& oldId : toRemove) {
            observations_.erase(oldId);
            idOrder_.erase(idOrder_.begin());
        }
    }

    LOG_DEBUG("TemporalStore: added observation {} for target {}", id, obs.targetId.value);
    return id;
}

std::string TemporalStore::add(const OCRResult& result, const TargetId& targetId, ContentSource source) {
    TemporalObservation obs;
    obs.targetId = targetId;
    obs.timestamp = result.timestamp > 0 ? result.timestamp : static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
    obs.source = source;
    obs.rawText = result.text;
    obs.normalizedText = result.normalizedText.empty() ? result.text : result.normalizedText;
    obs.confidence = result.confidence;
    TextNormalizer normalizer;
    obs.contentHash = hashToUint64(normalizer.computeHash(result.text));

    return add(obs);
}

std::optional<TemporalObservation> TemporalStore::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = observations_.find(id);
    if (it != observations_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<TemporalObservation> TemporalStore::getByTarget(const TargetId& targetId, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TemporalObservation> results;
    auto it = targetIndex_.find(targetId);

    if (it != targetIndex_.end()) {
        for (auto rit = it->second.rbegin(); 
             rit != it->second.rend() && results.size() < limit; ++rit) {
            auto obsIt = observations_.find(*rit);
            if (obsIt != observations_.end()) {
                results.push_back(obsIt->second);
            }
        }
    }

    return results;
}

std::vector<TemporalObservation> TemporalStore::getByTimeRange(uint64_t start, uint64_t end, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TemporalObservation> results;

    for (auto rit = idOrder_.rbegin(); 
         rit != idOrder_.rend() && results.size() < limit; ++rit) {
        auto it = observations_.find(*rit);
        if (it != observations_.end()) {
            if (it->second.timestamp >= start && it->second.timestamp <= end) {
                results.push_back(it->second);
            }
        }
    }

    return results;
}

std::vector<TemporalObservation> TemporalStore::getRecent(size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TemporalObservation> results;

    for (auto rit = idOrder_.rbegin(); 
         rit != idOrder_.rend() && results.size() < limit; ++rit) {
        auto it = observations_.find(*rit);
        if (it != observations_.end()) {
            results.push_back(it->second);
        }
    }

    return results;
}

bool TemporalStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = observations_.find(id);
    if (it == observations_.end()) {
        return false;
    }

    observations_.erase(it);

    auto idIt = std::find(idOrder_.begin(), idOrder_.end(), id);
    if (idIt != idOrder_.end()) {
        idOrder_.erase(idIt);
    }

    return true;
}

bool TemporalStore::removeByTarget(const TargetId& targetId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = targetIndex_.find(targetId);
    if (it == targetIndex_.end()) {
        return false;
    }

    for (const auto& id : it->second) {
        observations_.erase(id);
        auto idIt = std::find(idOrder_.begin(), idOrder_.end(), id);
        if (idIt != idOrder_.end()) {
            idOrder_.erase(idIt);
        }
    }

    targetIndex_.erase(it);
    return true;
}

bool TemporalStore::removeOlderThan(uint64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> toRemove;
    for (const auto& [id, obs] : observations_) {
        if (obs.timestamp < timestamp) {
            toRemove.push_back(id);
        }
    }

    for (const auto& id : toRemove) {
        auto it = observations_.find(id);
        if (it != observations_.end()) {
            targetIndex_[it->second.targetId].erase(
                std::remove(targetIndex_[it->second.targetId].begin(),
                           targetIndex_[it->second.targetId].end(), id),
                targetIndex_[it->second.targetId].end()
            );
            observations_.erase(it);
        }

        auto idIt = std::find(idOrder_.begin(), idOrder_.end(), id);
        if (idIt != idOrder_.end()) {
            idOrder_.erase(idIt);
        }
    }

    return !toRemove.empty();
}

bool TemporalStore::markIndexed(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = observations_.find(id);
    if (it == observations_.end()) {
        return false;
    }

    it->second.isIndexed = true;
    return true;
}

bool TemporalStore::markIndexed(const std::string& id, const std::vector<TextChunk>& chunks) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = observations_.find(id);
    if (it == observations_.end()) {
        return false;
    }

    it->second.isIndexed = true;
    it->second.chunks = chunks;
    return true;
}

size_t TemporalStore::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return observations_.size();
}

size_t TemporalStore::countByTarget(const TargetId& targetId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = targetIndex_.find(targetId);
    return it != targetIndex_.end() ? it->second.size() : 0;
}

size_t TemporalStore::countIndexed() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = 0;
    for (const auto& [id, obs] : observations_) {
        if (obs.isIndexed) count++;
    }
    return count;
}

size_t TemporalStore::countUnindexed() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = 0;
    for (const auto& [id, obs] : observations_) {
        if (!obs.isIndexed) count++;
    }
    return count;
}

void TemporalStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    observations_.clear();
    targetIndex_.clear();
    idOrder_.clear();
}

TemporalStore::Stats TemporalStore::getStats() const {
    Stats stats;
    stats.total = observations_.size();
    stats.indexed = countIndexed();
    stats.unindexed = countUnindexed();

    if (!idOrder_.empty()) {
        auto first = observations_.find(idOrder_.front());
        auto last = observations_.find(idOrder_.back());
        if (first != observations_.end()) stats.oldest = first->second.timestamp;
        if (last != observations_.end()) stats.newest = last->second.timestamp;
    }

    return stats;
}

// ============================================================================
// TextProcessingPipeline
// ============================================================================

TextProcessingPipeline::TextProcessingPipeline() = default;

TextProcessingPipeline::~TextProcessingPipeline() = default;

void TextProcessingPipeline::setNormalizerConfig(const TextProcessingConfig& /*config*/) {
    // The canonical TextNormalizer is stateless (no configurable instance);
    // config is accepted for API compatibility but has no effect.
}

void TextProcessingPipeline::setChunkerConfig(const TextProcessingConfig& config) {
    chunker_.setConfig(config);
}

void TextProcessingPipeline::addObserver(std::function<void(const TemporalObservation&)> observer) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    observers_.push_back(std::move(observer));
}

void TextProcessingPipeline::addChunkObserver(std::function<void(const TextChunk&)> observer) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    chunkObservers_.push_back(std::move(observer));
}

TemporalObservation TextProcessingPipeline::process(const std::string& rawText, const TargetId& targetId,
                                                const PageUrl& url, ContentSource source) {
    std::string normalized = normalizer_.normalize(rawText);
    uint64_t hash = hashToUint64(normalizer_.computeHash(normalized));

    if (deduplicationEnabled_ && deduplicator_.isDuplicate(normalized, hash)) {
        TemporalObservation obs;
        obs.targetId = targetId;
        obs.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
        obs.source = source;
        obs.rawText = rawText;
        obs.normalizedText = normalized;
        obs.contentHash = hash;
        obs.isDuplicate = true;
        return obs;
    }

    deduplicator_.markSeen(normalized, hash);

    TemporalObservation obs;
    obs.targetId = targetId;
    obs.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
    obs.source = source;
    obs.rawText = rawText;
    obs.normalizedText = normalized;
    obs.confidence = 1.0f;
    obs.contentHash = hash;
    obs.isDuplicate = false;
    obs.sourceUrl = url.value;

    if (indexingEnabled_) {
        auto chunks = chunker_.chunk(normalized, targetId, url, source);
        obs.chunks = chunks;
        obs.isIndexed = true;
        store_.add(obs);

        {
            std::lock_guard<std::mutex> lock(observerMutex_);
            for (const auto& chunk : chunks) {
                for (auto& cb : chunkObservers_) {
                    cb(chunk);
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(observerMutex_);
        for (auto& obsCb : observers_) {
            obsCb(obs);
        }
    }

    return obs;
}

TemporalObservation TextProcessingPipeline::process(const OCRResult& ocrResult, const TargetId& targetId) {
    return process(ocrResult.text, targetId, PageUrl(), ContentSource::OCR);
}

TemporalObservation TextProcessingPipeline::process(const std::string& rawText, ContentSource source) {
    return process(rawText, TargetId("unknown"), PageUrl(), source);
}

void TextProcessingPipeline::setDeduplicationEnabled(bool enabled) {
    deduplicationEnabled_ = enabled;
}

void TextProcessingPipeline::setIndexingEnabled(bool enabled) {
    indexingEnabled_ = enabled;
}

void TextProcessingPipeline::flush() {
    LOG_DEBUG("TextProcessingPipeline: flushed");
}

size_t TextProcessingPipeline::pendingCount() const {
    return store_.countUnindexed();
}

} // namespace edgemon
