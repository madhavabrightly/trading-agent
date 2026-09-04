#pragma once

#include "core/Types.hpp"
#include "extraction/DOMObserver.hpp"
#include <memory>
#include <vector>
#include <functional>

namespace edgemon {

class PageSession;

class DOMExtractor {
public:
    using TextCallback = std::function<void(const std::string& text, bool isNew)>;
    using ObservationCallback = std::function<void(const Observation& obs)>;

    explicit DOMExtractor(std::shared_ptr<PageSession> session);
    ~DOMExtractor();

    void initialize();
    void shutdown();

    std::string extractText();
    std::string extractText(const std::string& selector);
    std::vector<std::string> extractTexts(const std::vector<std::string>& selectors);

    Observation extractObservation();
    std::vector<Observation> extractObservations();

    void setTextCallback(TextCallback callback);
    void setObservationCallback(ObservationCallback callback);

    std::string getLastExtractedText() const { return lastText_; }
    uint64_t getLastHash() const { return lastHash_; }

    bool hasNewContent() const { return changeDetector_.hasChanged(); }

    void reset();
    void forceNextExtraction();

    struct ExtractionStats {
        uint32_t totalExtractions = 0;
        uint32_t newContent = 0;
        uint32_t duplicates = 0;
        uint32_t domChanges = 0;
        double avgExtractionTimeMs = 0;
    };

    ExtractionStats getStats() const;

private:
    void onDOMChange(const std::string& text, uint64_t hash);
    std::string injectObserverScript();

    std::shared_ptr<PageSession> session_;
    DOMChangeDetector changeDetector_;
    DOMObserver observer_;

    std::string lastText_;
    uint64_t lastHash_ = 0;

    TextCallback textCallback_;
    ObservationCallback observationCallback_;

    std::atomic<uint32_t> totalExtractions_{0};
    std::atomic<uint32_t> newContent_{0};
    std::atomic<uint32_t> duplicates_{0};
    std::atomic<uint32_t> domChanges_{0};

    std::chrono::steady_clock::time_point lastExtractionTime_;
};

class DOMExtractionPipeline {
public:
    DOMExtractionPipeline();
    ~DOMExtractionPipeline();

    void addExtractor(std::shared_ptr<DOMExtractor> extractor);
    void removeExtractor(const TargetId& targetId);

    void setObservationCallback(DOMExtractor::ObservationCallback callback);

    std::vector<Observation> processAll();

    size_t extractorCount() const;
    void shutdownAll();

private:
    std::vector<std::shared_ptr<DOMExtractor>> extractors_;
    DOMExtractor::ObservationCallback observationCallback_;
};

} // namespace edgemon
