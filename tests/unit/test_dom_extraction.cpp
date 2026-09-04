#include "extraction/DOMObserver.hpp"
#include "extraction/DOMExtractor.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace edgemon;

void test_dom_observer_creation() {
    std::cout << "Testing DOMObserver creation..." << std::endl;
    
    DOMObserver observer;
    
    std::cout << "  DOMObserver creation: PASS" << std::endl;
}

void test_dom_observer_generate_script() {
    std::cout << "Testing DOMObserver script generation..." << std::endl;
    
    DOMObserver observer;
    std::string script = observer.generateObserverScript();
    
    assert(!script.empty());
    assert(script.find("MutationObserver") != std::string::npos);
    assert(script.find("childList") != std::string::npos);
    assert(script.find("subtree") != std::string::npos);
    
    std::cout << "  Script length: " << script.length() << " chars" << std::endl;
    std::cout << "  DOMObserver script generation: PASS" << std::endl;
}

void test_dom_observer_debounced_script() {
    std::cout << "Testing DOMObserver debounced script..." << std::endl;
    
    DOMObserver observer;
    std::string script = observer.generateDebouncedObserverScript(500);
    
    assert(!script.empty());
    assert(script.find("500") != std::string::npos);
    
    std::cout << "  DOMObserver debounced script: PASS" << std::endl;
}

void test_dom_observer_extract_text_script() {
    std::cout << "Testing DOMObserver extract text script..." << std::endl;
    
    std::string script = DOMObserver::extractTextFromNode("body");
    
    assert(!script.empty());
    assert(script.find("document.querySelector") != std::string::npos);
    
    std::cout << "  DOMObserver extract text script: PASS" << std::endl;
}

void test_dom_observer_compute_hash() {
    std::cout << "Testing DOMObserver hash computation..." << std::endl;
    
    std::string text1 = "BTC: $104,250";
    std::string text2 = "BTC: $104,250";
    std::string text3 = "ETH: $3,450";
    
    uint64_t hash1 = DOMObserver::computeHash(text1);
    uint64_t hash2 = DOMObserver::computeHash(text2);
    uint64_t hash3 = DOMObserver::computeHash(text3);
    
    assert(hash1 != 0);
    assert(hash1 == hash2);
    assert(hash1 != hash3);
    
    std::cout << "  Hash('BTC: $104,250'): " << hash1 << std::endl;
    std::cout << "  Hash('ETH: $3,450'): " << hash3 << std::endl;
    std::cout << "  DOMObserver hash computation: PASS" << std::endl;
}

void test_dom_change_detector_basic() {
    std::cout << "Testing DOMChangeDetector basic..." << std::endl;
    
    DOMChangeDetector detector;
    detector.setDebounceMs(0);  // deterministic: no debounce for this sequence
    
    std::string text1 = "Initial content";
    std::string text2 = "Changed content";
    
    bool changed1 = detector.detect(text1);
    assert(changed1 == true);
    
    bool changed2 = detector.detect(text1);
    assert(changed2 == false);
    
    bool changed3 = detector.detect(text2);
    assert(changed3 == true);
    
    std::cout << "  DOMChangeDetector basic: PASS" << std::endl;
}

void test_dom_change_detector_debouncing() {
    std::cout << "Testing DOMChangeDetector debouncing..." << std::endl;
    
    DOMChangeDetector detector;
    detector.setDebounceMs(1000);
    
    detector.detect("Content A");
    
    bool rapid1 = detector.detect("Content B");
    bool rapid2 = detector.detect("Content C");
    
    assert(rapid1 == false);
    assert(rapid2 == false);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    
    bool afterDebounce = detector.detect("Content D");
    assert(afterDebounce == true);
    
    std::cout << "  DOMChangeDetector debouncing: PASS" << std::endl;
}

void test_dom_change_detector_hash_tracking() {
    std::cout << "Testing DOMChangeDetector hash tracking..." << std::endl;
    
    DOMChangeDetector detector;
    
    std::string text1 = "BTC: $104,250";
    uint64_t hash1 = DOMObserver::computeHash(text1);
    
    detector.detect(text1, hash1);
    
    assert(detector.getLastHash() == hash1);
    assert(detector.hasChanged() == true);
    
    bool same = detector.detect(text1, hash1);
    assert(same == false);
    assert(detector.hasChanged() == false);
    
    std::cout << "  DOMChangeDetector hash tracking: PASS" << std::endl;
}

void test_dom_change_detector_reset() {
    std::cout << "Testing DOMChangeDetector reset..." << std::endl;
    
    DOMChangeDetector detector;
    
    detector.detect("Content 1");
    detector.detect("Content 2");
    
    assert(detector.getChangeCount() == 1);
    
    detector.reset();
    
    bool afterReset = detector.detect("Content 1");
    assert(afterReset == true);
    
    std::cout << "  DOMChangeDetector reset: PASS" << std::endl;
}

void test_dom_change_detector_force_next() {
    std::cout << "Testing DOMChangeDetector forceNextChange..." << std::endl;
    
    DOMChangeDetector detector;
    
    detector.detect("Same content");
    bool duplicate = detector.detect("Same content");
    
    assert(duplicate == false);
    
    detector.forceNextChange();
    
    bool forced = detector.detect("Same content");
    assert(forced == true);
    
    std::cout << "  DOMChangeDetector forceNextChange: PASS" << std::endl;
}

void test_dom_change_detector_stats() {
    std::cout << "Testing DOMChangeDetector stats..." << std::endl;
    
    DOMChangeDetector detector;
    detector.setDebounceMs(0);  // deterministic: no debounce for this sequence
    
    detector.detect("Text 1");
    detector.detect("Text 1");
    detector.detect("Text 2");
    detector.detect("Text 2");
    detector.detect("Text 1");
    
    auto stats = detector.getStats();
    
    assert(stats.totalChanges == 2);
    assert(stats.duplicatesSkipped >= 1);
    
    std::cout << "  Total changes: " << stats.totalChanges << std::endl;
    std::cout << "  Duplicates skipped: " << stats.duplicatesSkipped << std::endl;
    std::cout << "  DOMChangeDetector stats: PASS" << std::endl;
}

void test_dom_change_detector_callback() {
    std::cout << "Testing DOMChangeDetector callbacks..." << std::endl;
    
    DOMChangeDetector detector;
    
    bool callbackCalled = false;
    bool callbackIsNew = false;
    
    detector.setChangeCallback([&callbackCalled, &callbackIsNew](const std::string& text, bool isNew) {
        callbackCalled = true;
        callbackIsNew = isNew;
    });
    
    detector.detect("New content");
    
    assert(callbackCalled == true);
    assert(callbackIsNew == true);
    
    callbackCalled = false;
    detector.detect("New content");
    
    assert(callbackCalled == false);
    
    std::cout << "  DOMChangeDetector callbacks: PASS" << std::endl;
}

void test_dom_change_detector_hash_callback() {
    std::cout << "Testing DOMChangeDetector hash callback..." << std::endl;
    
    DOMChangeDetector detector;
    
    bool hashCalled = false;
    
    detector.setHashCallback([&hashCalled](const std::string& text, uint64_t hash) {
        hashCalled = true;
        assert(hash != 0);
    });
    
    detector.detect("Content with hash");
    
    assert(hashCalled == true);
    
    std::cout << "  DOMChangeDetector hash callback: PASS" << std::endl;
}

void test_pipeline_flow() {
    std::cout << "Testing DOM change detection pipeline flow..." << std::endl;
    
    std::vector<std::pair<std::string, bool>> sequence = {
        {"Page loaded", true},
        {"Page loaded", false},
        {"Page loaded", false},
        {"BTC: $104,250", true},
        {"BTC: $104,250", false},
        {"BTC: $104,251", true},
        {"BTC: $104,251", false},
        {"ETH: $3,450", true},
    };
    
    DOMChangeDetector detector;
    detector.setDebounceMs(0);  // deterministic for the rapid synthetic sequence
    size_t newCount = 0;
    
    for (const auto& [text, expectedNew] : sequence) {
        bool isNew = detector.detect(text);
        if (isNew) newCount++;
        assert(isNew == expectedNew);
    }
    
    assert(newCount == 4);
    
    std::cout << "  Pipeline flow: PASS" << std::endl;
    std::cout << "  New content detected: " << newCount << "/8" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - DOM Extraction Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_dom_observer_creation();
    test_dom_observer_generate_script();
    test_dom_observer_debounced_script();
    test_dom_observer_extract_text_script();
    test_dom_observer_compute_hash();
    test_dom_change_detector_basic();
    test_dom_change_detector_debouncing();
    test_dom_change_detector_hash_tracking();
    test_dom_change_detector_reset();
    test_dom_change_detector_force_next();
    test_dom_change_detector_stats();
    test_dom_change_detector_callback();
    test_dom_change_detector_hash_callback();
    test_pipeline_flow();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All DOM extraction tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
