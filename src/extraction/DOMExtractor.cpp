#include "extraction/DOMExtractor.hpp"
#include "edge/PageSession.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>

namespace edgemon {

DOMExtractor::DOMExtractor(std::shared_ptr<PageSession> session)
    : session_(std::move(session))
    , lastExtractionTime_(std::chrono::steady_clock::now()) {
    
    changeDetector_.setChangeCallback([this](const std::string& text, bool isNew) {
        onDOMChange(text, 0);
    });
}

DOMExtractor::~DOMExtractor() {
    shutdown();
}

void DOMExtractor::initialize() {
    if (!session_) {
        LOG_ERROR("DOMExtractor: no session available");
        return;
    }

    std::string script = injectObserverScript();
    
    auto result = session_->executeJavaScriptSync(script, std::chrono::seconds(5));
    
    if (result) {
        LOG_INFO("DOMExtractor: observer injected successfully");
    } else {
        LOG_WARN("DOMExtractor: observer injection may have failed");
    }

    LOG_DEBUG("DOMExtractor: initialized");
}

void DOMExtractor::shutdown() {
    if (session_) {
        std::string disconnectScript = R"(
            (function() {
                if (window.__edgeMonitorObserver) {
                    window.__edgeMonitorObserver.disconnect();
                    delete window.__edgeMonitorObserver;
                }
            })()
        )";
        session_->executeJavaScript(disconnectScript);
    }
    
    LOG_DEBUG("DOMExtractor: shutdown");
}

std::string DOMExtractor::extractText() {
    return extractText("body");
}

std::string DOMExtractor::extractText(const std::string& selector) {
    if (!session_) {
        return "";
    }

    auto start = std::chrono::steady_clock::now();
    
    std::string script = DOMObserver::extractTextFromNode(selector);
    std::string text = session_->executeJavaScript(script);
    
    if (!text.empty()) {
        bool isNew = changeDetector_.detect(text);
        
        if (isNew) {
            lastText_ = text;
            lastHash_ = DOMObserver::computeHash(text);
            newContent_++;
        } else {
            duplicates_++;
        }
        
        totalExtractions_++;
    }
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    LOG_DEBUG("DOMExtractor: extracted {} chars in {}ms", text.length(), elapsed);
    
    return text;
}

std::vector<std::string> DOMExtractor::extractTexts(const std::vector<std::string>& selectors) {
    std::vector<std::string> results;
    
    for (const auto& selector : selectors) {
        results.push_back(extractText(selector));
    }
    
    return results;
}

Observation DOMExtractor::extractObservation() {
    Observation obs;
    
    if (!session_) {
        return obs;
    }

    obs.targetId = session_->getTargetId();
    obs.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    std::string script = R"(
        (function() {
            const body = document.body;
            if (!body) return JSON.stringify({text: '', structured: null});
            
            const text = body.innerText || body.textContent || '';
            
            const structured = {};
            
            const tables = body.querySelectorAll('table');
            tables.forEach((t, i) => {
                const cells = Array.from(t.querySelectorAll('td, th')).map(c => c.textContent.trim());
                if (cells.length > 0) {
                    structured['table_' + i] = cells;
                }
            });
            
            const inputs = body.querySelectorAll('input[value], textarea');
            inputs.forEach((el, i) => {
                if (el.value) {
                    structured['input_' + i] = el.value;
                }
            });
            
            const numbers = body.innerText.match(/[\d,]+\.?\d*/g);
            if (numbers) {
                structured['numbers'] = numbers.slice(0, 20);
            }
            
            return JSON.stringify({text: text.trim(), structured: structured});
        })()
    )";
    
    auto result = session_->executeJavaScriptSync(script, std::chrono::seconds(5));
    
    if (result && result->contains("result")) {
        try {
            std::string jsonStr = (*result)["result"]["value"].get<std::string>();
            
            auto json = nlohmann::json::parse(jsonStr);
            
            obs.rawText = json.value("text", "");
            obs.source = ContentSource::DOM;
            obs.contentHash = DOMObserver::computeHash(obs.rawText);
            obs.confidence = 1.0f;

            bool isNew = changeDetector_.detect(obs.rawText, obs.contentHash);
            obs.normalizedText = obs.rawText;
            
            if (isNew) {
                lastText_ = obs.rawText;
                lastHash_ = obs.contentHash;
                newContent_++;
                
                if (observationCallback_) {
                    observationCallback_(obs);
                }
            } else {
                duplicates_++;
            }
            
            totalExtractions_++;
            
        } catch (const std::exception& e) {
            LOG_ERROR("DOMExtractor: observation extraction error: {}", e.what());
        }
    }
    
    return obs;
}

std::vector<Observation> DOMExtractor::extractObservations() {
    std::vector<Observation> observations;
    
    auto obs = extractObservation();
    if (!obs.rawText.empty()) {
        observations.push_back(obs);
    }
    
    return observations;
}

void DOMExtractor::setTextCallback(TextCallback callback) {
    textCallback_ = std::move(callback);
}

void DOMExtractor::setObservationCallback(ObservationCallback callback) {
    observationCallback_ = std::move(callback);
}

void DOMExtractor::reset() {
    changeDetector_.reset();
    lastText_.clear();
    lastHash_ = 0;
    LOG_DEBUG("DOMExtractor: reset");
}

void DOMExtractor::forceNextExtraction() {
    changeDetector_.forceNextChange();
    LOG_DEBUG("DOMExtractor: forced next extraction");
}

void DOMExtractor::onDOMChange(const std::string& text, uint64_t hash) {
    if (text.empty()) {
        return;
    }

    domChanges_++;
    
    uint64_t contentHash = hash ? hash : DOMObserver::computeHash(text);
    
    bool isNew = changeDetector_.detect(text, contentHash);
    
    if (isNew) {
        lastText_ = text;
        lastHash_ = contentHash;
        newContent_++;
        
        LOG_DEBUG("DOMExtractor: new DOM content detected, {} chars", text.length());
        
        if (textCallback_) {
            try {
                textCallback_(text, true);
            } catch (const std::exception& e) {
                LOG_ERROR("DOMExtractor text callback error: {}", e.what());
            }
        }
    }
}

std::string DOMExtractor::injectObserverScript() {
    std::string script = R"(
        (function() {
            if (window.__edgeMonitorObserver) {
                window.__edgeMonitorObserver.disconnect();
            }
            
            const config = {
                childList: true,
                subtree: true,
                characterData: true,
                attributes: true,
                attributeOldValue: true
            };
            
            const IGNORED_TAGS = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'IFRAME', 'CANVAS']);
            
            function extractTextFromNode(node) {
                if (!node) return '';
                
                let text = '';
                
                if (node.nodeType === Node.TEXT_NODE) {
                    const trimmed = node.textContent.trim();
                    if (trimmed.length > 0) {
                        text += trimmed + ' ';
                    }
                } else if (node.nodeType === Node.ELEMENT_NODE) {
                    const tag = node.tagName;
                    if (IGNORED_TAGS.has(tag)) return '';
                    
                    const display = window.getComputedStyle(node).display;
                    if (display === 'none' || display === 'hidden') return '';
                    
                    for (const child of node.childNodes) {
                        text += extractTextFromNode(child);
                    }
                    
                    if (['P', 'DIV', 'LI', 'TR', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6'].includes(tag)) {
                        text += '\n';
                    }
                }
                
                return text;
            }
            
            function computeHash(text) {
                let hash = 5381;
                for (let i = 0; i < text.length; i++) {
                    hash = ((hash << 5) + hash) + text.charCodeAt(i);
                    hash = hash & hash;
                }
                return Math.abs(hash);
            }
            
            let pendingChanges = [];
            let debounceTimer = null;
            const DEBOUNCE_MS = )" + std::to_string(changeDetector_.getDebounceMs()) + R"(;
            
            function processChanges() {
                if (pendingChanges.length === 0) return;
                
                const text = extractTextFromNode(document.body);
                const hash = computeHash(text);
                
                if (typeof window.onDOMChange === 'function') {
                    window.onDOMChange({
                        type: 'dom_change',
                        text: text,
                        hash: hash,
                        timestamp: Date.now()
                    });
                }
                
                pendingChanges = [];
            }
            
            const observer = new MutationObserver((mutations) => {
                for (const mutation of mutations) {
                    if (mutation.type === 'childList' || 
                        mutation.type === 'characterData' ||
                        mutation.type === 'attributes') {
                        pendingChanges.push(Date.now());
                    }
                }
                
                if (debounceTimer) clearTimeout(debounceTimer);
                debounceTimer = setTimeout(processChanges, DEBOUNCE_MS);
            });
            
            observer.observe(document.documentElement, config);
            
            const initialText = extractTextFromNode(document.body);
            const initialHash = computeHash(initialText);
            
            window.__edgeMonitorObserver = observer;
            window.__edgeMonitorHash = initialHash;
            
            if (typeof window.onDOMChange === 'function') {
                window.onDOMChange({
                    type: 'dom_change',
                    text: initialText,
                    hash: initialHash,
                    timestamp: Date.now()
                });
            }
            
            return {
                observer: observer,
                hash: initialHash,
                text: initialText,
                disconnect: function() {
                    if (debounceTimer) clearTimeout(debounceTimer);
                    observer.disconnect();
                    delete window.__edgeMonitorObserver;
                    delete window.__edgeMonitorHash;
                }
            };
        })()
    )";
    
    return script;
}

DOMExtractor::ExtractionStats DOMExtractor::getStats() const {
    ExtractionStats stats;
    stats.totalExtractions = totalExtractions_.load();
    stats.newContent = newContent_.load();
    stats.duplicates = duplicates_.load();
    stats.domChanges = domChanges_.load();
    return stats;
}

DOMExtractionPipeline::DOMExtractionPipeline() = default;

DOMExtractionPipeline::~DOMExtractionPipeline() {
    shutdownAll();
}

void DOMExtractionPipeline::addExtractor(std::shared_ptr<DOMExtractor> extractor) {
    if (!extractor) return;
    
    extractors_.push_back(extractor);
    
    if (observationCallback_) {
        extractor->setObservationCallback(observationCallback_);
    }
    
    extractor->initialize();
    
    LOG_DEBUG("DOMExtractionPipeline: added extractor for target {}", 
              extractor->getLastExtractedText().empty() ? "unknown" : "ready");
}

void DOMExtractionPipeline::removeExtractor(const TargetId& targetId) {
    extractors_.erase(
        std::remove_if(extractors_.begin(), extractors_.end(),
            [&targetId](const std::shared_ptr<DOMExtractor>& ext) {
                return ext->getLastExtractedText().empty();
            }),
        extractors_.end()
    );
}

void DOMExtractionPipeline::setObservationCallback(DOMExtractor::ObservationCallback callback) {
    observationCallback_ = std::move(callback);
    
    for (auto& extractor : extractors_) {
        extractor->setObservationCallback(observationCallback_);
    }
}

std::vector<Observation> DOMExtractionPipeline::processAll() {
    std::vector<Observation> allObservations;
    
    for (auto& extractor : extractors_) {
        auto observations = extractor->extractObservations();
        allObservations.insert(allObservations.end(), 
                            observations.begin(), observations.end());
    }
    
    return allObservations;
}

size_t DOMExtractionPipeline::extractorCount() const {
    return extractors_.size();
}

void DOMExtractionPipeline::shutdownAll() {
    for (auto& extractor : extractors_) {
        extractor->shutdown();
    }
    extractors_.clear();
    LOG_INFO("DOMExtractionPipeline: shutdown complete");
}

} // namespace edgemon
