#include "extraction/DOMObserver.hpp"
#include "logger.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace edgemon {

DOMObserver::DOMObserver() = default;

DOMObserver::~DOMObserver() = default;

std::string DOMObserver::baseObserverScript() {
    return R"(
(function() {
    if (window.__edgeMonitorObserver) {
        window.__edgeMonitorObserver.disconnect();
    }
    
    const config = {
        childList: true,
        subtree: true,
        characterData: true,
        attributes: true,
        attributeFilter: ['class', 'style', 'disabled', 'value', 'data-value', 'aria-label']
    };
    
    const IGNORED_TAGS = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'IFRAME', 'CANVAS', 'SVG', 'MATH']);
    const IGNORED_CLASSES = new Set(['ad-', 'ads-', 'banner', 'popup', 'modal-overlay']);
    
    function shouldIgnoreNode(node) {
        if (IGNORED_TAGS.has(node.nodeName)) return true;
        if (node.nodeType === Node.ELEMENT_NODE) {
            for (const cls of IGNORED_CLASSES) {
                if (node.className && node.className.includes && node.className.includes(cls)) {
                    return true;
                }
            }
        }
        return false;
    }
    
    function extractRelevantText(node, maxLength = 5000) {
        if (!node) return '';
        
        let text = '';
        const walker = document.createTreeWalker(
            node,
            NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT,
            {
                acceptNode: function(node) {
                    if (node.nodeType === Node.TEXT_NODE) {
                        const parent = node.parentElement;
                        if (parent && shouldIgnoreNode(parent)) {
                            return NodeFilter.FILTER_REJECT;
                        }
                        return NodeFilter.FILTER_ACCEPT;
                    }
                    if (node.nodeType === Node.ELEMENT_NODE) {
                        if (shouldIgnoreNode(node)) {
                            return NodeFilter.FILTER_REJECT;
                        }
                        const tag = node.tagName;
                        if (['SCRIPT', 'STYLE', 'NOSCRIPT'].includes(tag)) {
                            return NodeFilter.FILTER_REJECT;
                        }
                        return NodeFilter.FILTER_ACCEPT;
                    }
                    return NodeFilter.FILTER_REJECT;
                }
            }
        );
        
        const nodes = [];
        let current;
        while (current = walker.nextNode()) {
            if (current.nodeType === Node.TEXT_NODE) {
                const trimmed = current.textContent.trim();
                if (trimmed.length > 0) {
                    nodes.push(trimmed);
                }
            } else if (current.tagName === 'INPUT' || current.tagName === 'TEXTAREA') {
                const val = current.value || '';
                if (val.trim().length > 0) {
                    nodes.push('[' + current.tagName + ':' + current.name + ']=' + val);
                }
            }
        }
        
        text = nodes.join(' | ');
        
        if (text.length > maxLength) {
            text = text.substring(0, maxLength) + '...[truncated]';
        }
        
        return text;
    }
    
    function extractStructuredData(node) {
        const data = {};
        
        const tables = node.querySelectorAll('table');
        tables.forEach((table, i) => {
            const rows = [];
            const cells = table.querySelectorAll('td, th');
            cells.forEach(cell => {
                const text = cell.textContent.trim();
                if (text) rows.push(text);
            });
            if (rows.length > 0) {
                data['table_' + i] = rows.join(', ');
            }
        });
        
        const priceElements = node.querySelectorAll('[class*="price"], [class*="value"], [class*="BTC"], [class*="ETH"], [data-price], [data-value]');
        priceElements.forEach(el => {
            const text = el.textContent.trim();
            if (text && /[\d,.$]/.test(text)) {
                const key = el.className || el.tagName;
                data['price_' + key.substring(0, 20)] = text;
            }
        });
        
        return data;
    }
    
    function computeHash(text) {
        let hash = 0;
        for (let i = 0; i < text.length; i++) {
            const char = text.charCodeAt(i);
            hash = ((hash << 5) - hash) + char;
            hash = hash & hash;
        }
        return Math.abs(hash);
    }
    
    let lastHash = 0;
    let pendingTimeout = null;
    
    const observer = new MutationObserver((mutations) => {
        if (pendingTimeout) return;
        
        pendingTimeout = setTimeout(() => {
            pendingTimeout = null;
            
            const text = extractRelevantText(document.body);
            const newHash = computeHash(text);
            
            if (newHash !== lastHash) {
                lastHash = newHash;
                
                const payload = {
                    type: 'dom_change',
                    hash: newHash,
                    text: text,
                    timestamp: Date.now(),
                    structured: extractStructuredData(document.body)
                };
                
                if (typeof window.onDOMChange === 'function') {
                    window.onDOMChange(payload);
                }
            }
        }, )" + std::to_string(300) + R"();
    });
    
    observer.observe(document.documentElement, config);
    
    window.__edgeMonitorObserver = observer;
    
    const initialText = extractRelevantText(document.body);
    const initialHash = computeHash(initialText);
    
    return {
        observer: observer,
        hash: initialHash,
        text: initialText,
        disconnect: function() {
            if (pendingTimeout) clearTimeout(pendingTimeout);
            observer.disconnect();
            delete window.__edgeMonitorObserver;
        }
    };
})();
)";
}

std::string DOMObserver::debouncedObserverScript(int debounceMs) {
    std::string script = baseObserverScript();
    
    size_t pos = script.rfind(std::to_string(300));
    if (pos != std::string::npos) {
        script.replace(pos, std::to_string(300).length(), std::to_string(debounceMs));
    }
    
    return script;
}

std::string DOMObserver::generateObserverScript() {
    return baseObserverScript();
}

std::string DOMObserver::generateDebouncedObserverScript(int debounceMs) {
    return debouncedObserverScript(debounceMs);
}

std::string DOMObserver::extractTextFromNode(const std::string& selector) {
    return R"(
(function() {
    const node = document.querySelector(')" + selector + R"(');
    if (!node) return '';
    
    const IGNORED = /^(script|style|noscript|iframe|canvas|svg)$/i;
    
    function getText(element) {
        let text = '';
        for (const child of element.childNodes) {
            if (child.nodeType === Node.TEXT_NODE) {
                const trimmed = child.textContent.trim();
                if (trimmed) text += trimmed + ' ';
            } else if (child.nodeType === Node.ELEMENT_NODE) {
                if (!IGNORED.test(child.tagName)) {
                    text += getText(child) + ' ';
                }
            }
        }
        return text.trim();
    }
    
    return getText(node);
})()
)";
}

std::string DOMObserver::extractVisibleText(const std::string& selector) {
    return R"(
(function() {
    const node = document.querySelector(')" + selector + R"(');
    if (!node) return '';
    
    function getVisibleText(element) {
        const style = window.getComputedStyle(element);
        if (style.display === 'none' || style.visibility === 'hidden') return '';
        
        let text = '';
        for (const child of element.childNodes) {
            if (child.nodeType === Node.TEXT_NODE) {
                text += child.textContent;
            } else if (child.nodeType === Node.ELEMENT_NODE) {
                text += getVisibleText(child);
            }
        }
        return text;
    }
    
    return getVisibleText(node).replace(/\s+/g, ' ').trim();
})()
)";
}

uint64_t DOMObserver::computeHash(const std::string& text) {
    if (text.empty()) return 0;
    
#ifdef _WIN32
    uint64_t hash = 5381;
    for (unsigned char c : text) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash & 0xFFFFFFFFFFFFFFFF;
#else
    uint64_t hash = 5381;
    for (unsigned char c : text) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
#endif
}

DOMChangeDetector::DOMChangeDetector()
    : lastChangeTime_(std::chrono::steady_clock::now()) {
}

DOMChangeDetector::~DOMChangeDetector() = default;

void DOMChangeDetector::setChangeCallback(ChangeCallback callback) {
    changeCallback_ = std::move(callback);
}

void DOMChangeDetector::setHashCallback(DOMObserver::ChangeCallback callback) {
    hashCallback_ = std::move(callback);
}

bool DOMChangeDetector::detect(const std::string& text) {
    uint64_t hash = DOMObserver::computeHash(text);
    return detect(text, hash);
}

bool DOMChangeDetector::detect(const std::string& text, uint64_t hash) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastChangeTime_).count();
    
    bool isNew = false;
    
    if (hash != lastHash_) {
        // First-ever content (lastHash_ == 0) must bypass debounce: a fresh
        // page's initial text is new content, not a mutation storm.
        if (lastHash_ != 0 && elapsed < debounceMs_) {
            LOG_DEBUG("DOMChangeDetector: debouncing change ({}ms < {}ms)", elapsed, debounceMs_);
            return false;
        }
        
        auto it = std::find(recentHashes_.begin(), recentHashes_.end(), hash);
        if (it != recentHashes_.end()) {
            duplicatesSkipped_++;
            LOG_DEBUG("DOMChangeDetector: duplicate hash detected");
            return false;
        }
        
        isNew = true;
        lastHash_ = hash;
        lastChangeTime_ = now;
        changeCount_++;
        hasChanged_ = true;
        
        recentHashes_.push_back(hash);
        if (recentHashes_.size() > maxRecentHashes_) {
            recentHashes_.erase(recentHashes_.begin());
        }
        
        LOG_DEBUG("DOMChangeDetector: new content detected, hash={}", hash);
    } else {
        hashMatches_++;
        hasChanged_ = false;
    }
    
    if (isNew && changeCallback_) {
        try {
            changeCallback_(text, isNew);
        } catch (const std::exception& e) {
            LOG_ERROR("DOMChangeDetector callback error: {}", e.what());
        }
    }
    
    if (hashCallback_) {
        try {
            hashCallback_(text, hash);
        } catch (const std::exception& e) {
            LOG_ERROR("DOMChangeDetector hash callback error: {}", e.what());
        }
    }
    
    return isNew;
}

void DOMChangeDetector::reset() {
    lastHash_ = 0;
    lastChangeTime_ = std::chrono::steady_clock::now();
    hasChanged_ = false;
    recentHashes_.clear();
    LOG_DEBUG("DOMChangeDetector: reset");
}

void DOMChangeDetector::forceNextChange() {
    lastHash_ = 0;
    lastChangeTime_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(debounceMs_ + 1);
    recentHashes_.clear();
    LOG_DEBUG("DOMChangeDetector: forced next change");
}

DOMChangeDetector::DetectionStats DOMChangeDetector::getStats() const {
    DetectionStats stats;
    stats.lastHash = lastHash_;
    stats.totalChanges = changeCount_.load();
    stats.duplicatesSkipped = duplicatesSkipped_.load();
    stats.hashMatches = hashMatches_.load();
    return stats;
}

} // namespace edgemon
