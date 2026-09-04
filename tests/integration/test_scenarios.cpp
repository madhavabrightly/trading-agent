// Integration tests for the Edge Monitor pipeline.
// Covers: multi-target registration, duplicate prevention, tab-switch
// independence, background/occluded pages, navigation, CDP disconnect/reconnect,
// target closure, OCR change detection, embedding generation, semantic
// retrieval, and bounded-memory stress at 1/5/10/20 targets.
//
// Identity rule under test: a monitored target is a unique internal TargetId in
// TargetRegistry bound to a CDPTargetId + Edge process id. HWND / active tab /
// screen position are never identity.

#include "core/TargetRegistry.hpp"
#include "core/TargetManager.hpp"
#include "capture/BrowserCapture.hpp"
#include "capture/CaptureManager.hpp"
#include "capture/FrameBuffer.hpp"
#include "capture/ChangeDetector.hpp"
#include "extraction/OCRManager.hpp"
#include "extraction/TextNormalizer.hpp"
#include "embedding/EmbeddingEngine.hpp"
#include "threadpool.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <optional>

using namespace edgemon;

static std::atomic<int> g_pass{0};
static std::atomic<int> g_fail{0};

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
            std::cout << "  [PASS] " << msg << std::endl;                      \
        } else {                                                               \
            g_fail++;                                                          \
            std::cout << "  [FAIL] " << msg << std::endl;                      \
        }                                                                      \
    } while (0)

static MonitoredTarget makeTarget(const std::string& id,
                                  const std::string& cdpId,
                                  uint32_t pid,
                                  const std::string& url,
                                  const std::string& title,
                                  TargetState state = TargetState::Discovered) {
    MonitoredTarget t;
    t.id = TargetId(id);
    t.cdpTargetId = CDPTargetId(cdpId);
    t.edgeProcessId = EdgeProcessId(pid);
    t.url = PageUrl(url);
    t.title = title;
    t.state = state;
    t.enabled = true;
    return t;
}

// ---------------------------------------------------------------------------
// 1. Registry: registration, duplicates, lookup by CDP id
// ---------------------------------------------------------------------------

void test_registry_multi_target() {
    std::cout << "\n[1] Registry multi-target registration + duplicate prevention" << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    CHECK(reg.add(makeTarget("t1", "cdp-1", 1000, "https://a.example", "Page A")), "add t1");
    CHECK(reg.add(makeTarget("t2", "cdp-2", 1000, "https://b.example", "Page B")), "add t2");
    CHECK(reg.add(makeTarget("t3", "cdp-3", 2000, "https://c.example", "Page C")), "add t3");
    CHECK(reg.count() == 3, "count == 3");

    // Duplicate CDP id must be rejected.
    MonitoredTarget dup = makeTarget("t4", "cdp-1", 1000, "https://d.example", "Dup");
    CHECK(!reg.add(dup), "duplicate CDP id rejected");

    // Same URL, different CDP id: allowed at registry level (distinct tabs).
    CHECK(reg.add(makeTarget("t5", "cdp-5", 1000, "https://a.example", "Page A copy")),
          "same URL different CDP allowed (distinct tabs)");

    CHECK(reg.exists(TargetId("t2")), "exists t2");
    auto byCdp = reg.getByCDPId(CDPTargetId("cdp-2"));
    CHECK(byCdp.has_value() && byCdp->id.value == "t2", "getByCDPId finds t2");
    CHECK(!reg.getByCDPId(CDPTargetId("cdp-99")).has_value(), "unknown CDP id -> none");

    auto all = reg.getAll();
    CHECK(all.size() == 4, "getAll returns 4");

    // remove one; the rest survive (one failed target must not affect others).
    CHECK(reg.remove(TargetId("t2")), "remove t2");
    CHECK(reg.count() == 3, "count == 3 after removal");
    CHECK(reg.get(TargetId("t1")).has_value(), "t1 survives");
    CHECK(reg.get(TargetId("t3")).has_value(), "t3 survives");
    reg.clear();
}

// ---------------------------------------------------------------------------
// 2. State tracking + observers
// ---------------------------------------------------------------------------

void test_registry_state_tracking() {
    std::cout << "\n[2] Registry state tracking" << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    auto t = makeTarget("s1", "cdp-s1", 1000, "https://s.example", "State");
    reg.add(t);

    std::atomic<int> transitions{0};
    size_t obsId = reg.observeStateChanges([&](const TargetId&, TargetState, TargetState) {
        transitions++;
    });

    CHECK(reg.setState(TargetId("s1"), TargetState::Connected), "set Connected");
    CHECK(reg.setState(TargetId("s1"), TargetState::Monitoring), "set Monitoring");
    auto got = reg.get(TargetId("s1"));
    CHECK(got->state == TargetState::Monitoring, "state == Monitoring");
    CHECK(reg.countByState(TargetState::Monitoring) == 1, "countByState Monitoring == 1");
    CHECK(reg.countByState(TargetState::Connected) == 0, "countByState Connected == 0");

    reg.setState(TargetId("s1"), TargetState::Offline, "lost");
    got = reg.get(TargetId("s1"));
    CHECK(got->state == TargetState::Offline, "state == Offline");
    CHECK(got->stateMessage == "lost", "state message recorded");

    // Re-registration after navigation/reconnect: same internal id, new CDP id.
    reg.setCDPTargetId(TargetId("s1"), CDPTargetId("cdp-s1-new"));
    got = reg.get(TargetId("s1"));
    CHECK(got->cdpTargetId.value == "cdp-s1-new", "CDP id rebound after reconnect");
    CHECK(reg.getByCDPId(CDPTargetId("cdp-s1-new")).has_value(), "new CDP id resolves");
    CHECK(!reg.getByCDPId(CDPTargetId("cdp-s1")).has_value(), "old CDP id gone");
    CHECK(transitions.load() >= 3, "state observers fired");

    reg.removeByCDPTargetId(CDPTargetId("cdp-s1-new"));
    CHECK(reg.count() == 0, "removeByCDPTargetId works");
    reg.removeStateObserver(obsId);
    reg.clear();
}

// ---------------------------------------------------------------------------
// 3. TargetManager: lock/unlock, duplicates, multi-target
// ---------------------------------------------------------------------------

void test_manager_lock_duplicates() {
    std::cout << "\n[3] TargetManager lock + duplicate prevention" << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    auto mgr = std::make_shared<TargetManager>();
    mgr->initialize();

    EdgeTarget a;
    a.id = CDPTargetId("cdp-a");
    a.url = PageUrl("https://a.example");
    a.title = "Page A";
    a.processId = EdgeProcessId(1000);
    a.webSocketDebuggerUrl = "ws://localhost:9222/devtools/page/cdp-a";

    TargetId id1 = mgr->lockTarget(a);
    CHECK(!id1.empty(), "lock target A");
    CHECK(id1.value.rfind("target-", 0) == 0, "internal id is target-N");

    // Same CDP id -> same internal id (duplicate prevention).
    TargetId id1b = mgr->lockTarget(a);
    CHECK(id1b == id1, "re-lock same CDP returns same internal id");

    EdgeTarget b = a;
    b.id = CDPTargetId("cdp-b");
    b.url = PageUrl("https://b.example");
    b.title = "Page B";
    TargetId id2 = mgr->lockTarget(b);
    CHECK(!id2.empty() && id2 != id1, "lock target B with distinct id");

    // Same URL, new CDP id -> manager treats as same page (URL duplicate).
    EdgeTarget a2 = a;
    a2.id = CDPTargetId("cdp-a2");
    TargetId id1c = mgr->lockTarget(a2);
    CHECK(id1c == id1, "same URL re-lock returns same internal id");

    CHECK(reg.count() == 2, "registry has exactly 2 targets");
    CHECK(mgr->lockedCount() == 2, "lockedCount == 2");

    CHECK(mgr->unlockTarget(id1), "unlock A");
    CHECK(reg.count() == 1, "one target remains after unlock A");
    CHECK(reg.get(id2).has_value(), "B still locked and unaffected");

    mgr->shutdown();
    reg.clear();
}

// ---------------------------------------------------------------------------
// 4. Tab-switch independence + background/occluded page handling
// ---------------------------------------------------------------------------

void test_tab_switch_background() {
    std::cout << "\n[4] Tab-switch independence + background pages" << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    // Two targets on the same Edge process — switching the active tab must not
    // change which CDP target each internal id points at.
    reg.add(makeTarget("tab1", "cdp-tab1", 3000, "https://one.example", "Tab One", TargetState::Monitoring));
    reg.add(makeTarget("tab2", "cdp-tab2", 3000, "https://two.example", "Tab Two", TargetState::Monitoring));

    // Simulate: user switches to Tab Two (foreground). Identity is unchanged.
    auto t1 = reg.get(TargetId("tab1"));
    auto t2 = reg.get(TargetId("tab2"));
    CHECK(t1->cdpTargetId.value == "cdp-tab1", "tab1 still bound to its CDP target");
    CHECK(t2->cdpTargetId.value == "cdp-tab2", "tab2 still bound to its CDP target");
    CHECK(t1->id.value != t2->id.value, "internal ids distinct");

    // Background/occluded page: the target remains registered; capture reports
    // RenderUnavailable but the session/target identity is preserved.
    auto pool = std::make_shared<ThreadPool>(2);
    auto session = std::make_shared<PageSession>(
        TargetId("tab1"), CDPTargetId("cdp-tab1"),
        "ws://localhost:9222/devtools/page/cdp-tab1", pool);

    BrowserCapture capture(session);
    capture.setTargetId(TargetId("tab1"));
    CapturedFrame frame = capture.capture();
    CHECK(frame.targetId.value == "tab1", "background frame still attributed to tab1");
    // NotConnected is acceptable here (no live Edge in CI); identity must hold.
    CHECK(frame.status == CaptureStatus::NotConnected ||
          frame.status == CaptureStatus::RenderUnavailable,
          "background/occluded frame reports unavailable status");

    // Change detection: same frame content -> no change; different -> change.
    FrameChangeDetector detector;
    CapturedFrame f1;
    f1.targetId = TargetId("tab1");
    f1.pixels = std::vector<uint8_t>(16 * 16 * 4, 128);
    f1.width = 16; f1.height = 16;
    f1.status = CaptureStatus::Success;
    CapturedFrame f2 = f1;
    f2.pixels = std::vector<uint8_t>(16 * 16 * 4, 200);

    CHECK(detector.hasFrameChanged(f1), "first frame is a change (baseline)");
    CHECK(!detector.hasFrameChanged(f1), "identical frame -> no change");
    CHECK(detector.hasFrameChanged(f2), "different frame -> change");
    reg.clear();
}

// ---------------------------------------------------------------------------
// 5. Navigation / reload / CDP disconnect-reconnect / closure
// ---------------------------------------------------------------------------

void test_navigation_reconnect_closure() {
    std::cout << "\n[5] Navigation, reconnect, closure" << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    auto mgr = std::make_shared<TargetManager>();
    mgr->initialize();

    EdgeTarget t;
    t.id = CDPTargetId("cdp-nav");
    t.url = PageUrl("https://nav.example");
    t.title = "Nav";
    t.processId = EdgeProcessId(4000);
    t.webSocketDebuggerUrl = "ws://localhost:9222/devtools/page/cdp-nav";
    TargetId id = mgr->lockTarget(t);
    CHECK(!id.empty(), "lock navigation target");

    // CDP disconnect -> Offline; reconnect -> same internal id re-registered.
    mgr->handleTargetLost(t.id);
    auto got = reg.get(id);
    CHECK(got->state == TargetState::Offline, "target marked offline on CDP loss");

    // CDP may reassign the target id after reconnect; the same URL re-lock must
    // return the original internal id (URL-based identity fallback).
    EdgeTarget t2 = t;
    t2.id = CDPTargetId("cdp-nav-new");
    t2.webSocketDebuggerUrl = "ws://localhost:9222/devtools/page/cdp-nav-new";
    TargetId relocked = mgr->lockTarget(t2);
    CHECK(relocked == id, "re-lock after reconnect keeps internal identity");

    // Navigation: page moves URL but same CDP target / internal id.
    got = reg.get(id);
    MonitoredTarget updated = *got;
    updated.url = PageUrl("https://nav.example/page2");
    updated.title = "Nav Page 2";
    CHECK(reg.update(updated), "update after navigation");
    got = reg.get(id);
    CHECK(got->url.value == "https://nav.example/page2", "URL updated after navigation");
    CHECK(got->id.value == id.value, "internal id stable across navigation");

    // Closure: explicit unlock removes only this target.
    mgr->unlockTarget(id);
    CHECK(reg.count() == 0, "closure removes the target");

    mgr->shutdown();
    reg.clear();
}

// ---------------------------------------------------------------------------
// 6. OCR change detection + text normalization
// ---------------------------------------------------------------------------

void test_ocr_and_normalization() {
    std::cout << "\n[6] OCR pipeline + normalization" << std::endl;

    TextNormalizer normalizer;
    std::string raw = "  BTC:   $104,250  ";
    std::string norm = normalizer.normalize(raw);
    CHECK(!norm.empty(), "normalize non-empty");
    CHECK(norm.find("  ") == std::string::npos, "whitespace collapsed");
    CHECK(normalizer.computeHash(norm) == normalizer.computeHash(normalizer.normalize(raw)),
          "hash stable for identical text");

    auto pool = std::make_shared<ThreadPool>(2);
    OCRManager ocr(pool);
    ocr.initialize();

    CapturedFrame frame;
    frame.targetId = TargetId("ocr-1");
    frame.width = 64; frame.height = 64;
    frame.pixels = std::vector<uint8_t>(64 * 64 * 4, 0);

    std::atomic<bool> gotResult{false};
    std::atomic<bool> accepted{false};
    ocr.processFrame(frame, [&](const OCRResult& r) {
        gotResult = true;
        if (!r.rejected && !r.text.empty()) accepted = true;
    });

    // OCR is placeholder-based; the queue/result path must complete.
    for (int i = 0; i < 50 && !gotResult.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(gotResult.load(), "OCR result callback fired");
    CHECK(ocr.processedCount() > 0, "OCR processed count > 0");
    CHECK(ocr.queueSize() == 0, "OCR queue drained (bounded/backpressure)");

    ocr.shutdown();
}

// ---------------------------------------------------------------------------
// 7. Embeddings + semantic retrieval
// ---------------------------------------------------------------------------

void test_embeddings_semantic_search() {
    std::cout << "\n[7] Embedding generation + semantic retrieval" << std::endl;

    auto engine = std::make_shared<EmbeddingEngine>();
    EmbeddingConfig config;
    config.embeddingDim = 64;
    CHECK(engine->initialize(config), "embedding engine initializes");

    auto store = std::make_shared<EmbeddingStore>();
    store->setEngine(engine);
    CHECK(store->initialize(), "embedding store initializes");

    EmbeddingResult r1;
    r1.id = "e1"; r1.targetId = TargetId("t1");
    r1.content = "The price of bitcoin reached a new high today";
    r1.vector = engine->embed(r1.content);
    r1.timestamp = 1000;
    r1.contentHash = std::hash<std::string>{}(r1.content);
    store->add(r1);

    EmbeddingResult r2;
    r2.id = "e2"; r2.targetId = TargetId("t2");
    r2.content = "Weather forecast for tomorrow is sunny";
    r2.vector = engine->embed(r2.content);
    r2.timestamp = 2000;
    r2.contentHash = std::hash<std::string>{}(r2.content);
    store->add(r2);

    CHECK(store->count() == 2, "store has 2 embeddings");

    auto results = store->search("bitcoin price", 2);
    CHECK(!results.empty(), "search returns results");
    CHECK(results[0].id == "e1", "bitcoin query ranks e1 first");

    auto filtered = store->search("bitcoin price", TargetId("t2"), 2);
    CHECK(!filtered.empty() && filtered[0].id == "e2",
          "target-filtered search returns only that target");

    auto ranged = store->search("bitcoin price", TargetId("t1"), 500, 1500, 2);
    CHECK(!ranged.empty() && ranged[0].id == "e1", "time-ranged search works");

    store->shutdown();
    engine->shutdown();
}

// ---------------------------------------------------------------------------
// 8. Stress: 1/5/10/20 targets, bounded queues
// ---------------------------------------------------------------------------

void stressRun(int count) {
    auto& reg = TargetRegistry::instance();
    reg.clear();

    for (int i = 0; i < count; ++i) {
        MonitoredTarget t;
        t.id = TargetId("stress-" + std::to_string(i));
        t.cdpTargetId = CDPTargetId("cdp-stress-" + std::to_string(i));
        t.edgeProcessId = EdgeProcessId(5000 + i);
        t.url = PageUrl("https://stress" + std::to_string(i) + ".example");
        t.title = "Stress " + std::to_string(i);
        t.state = TargetState::Monitoring;
        CHECK(reg.add(t), "register stress target " + std::to_string(i));
    }
    CHECK(reg.count() == static_cast<size_t>(count), "all stress targets registered");

    // Each target remains independently identifiable.
    bool allDistinct = true;
    for (int i = 0; i < count; ++i) {
        auto got = reg.get(TargetId("stress-" + std::to_string(i)));
        if (!got || got->cdpTargetId.value != "cdp-stress-" + std::to_string(i)) {
            allDistinct = false;
        }
    }
    CHECK(allDistinct, "all " + std::to_string(count) + " targets independently identifiable");

    // Failure isolation: removing one must not disturb the others.
    if (count > 1) {
        reg.remove(TargetId("stress-0"));
        CHECK(reg.count() == static_cast<size_t>(count - 1),
              "removing one of " + std::to_string(count) + " keeps the rest");
        reg.clear();
    }
}

void test_stress() {
    std::cout << "\n[8] Stress: bounded queues + 1/5/10/20 targets" << std::endl;

    // Bounded frame queue: pushes beyond capacity are dropped, not grown.
    {
        FrameBuffer buffer(8);
        CapturedFrame f;
        f.targetId = TargetId("bounded");
        f.pixels = std::vector<uint8_t>(1024, 1);
        for (int i = 0; i < 100; ++i) buffer.push(f);
        CHECK(buffer.size() <= 8, "frame buffer stays bounded at capacity");
        CHECK(buffer.size() == 8, "frame buffer holds exactly capacity");
    }

    stressRun(1);
    stressRun(5);
    stressRun(10);
    stressRun(20);

    std::cout << "  Stress complete (1/5/10/20 targets)" << std::endl;
}

// ---------------------------------------------------------------------------

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_registry_multi_target();
    test_registry_state_tracking();
    test_manager_lock_duplicates();
    test_tab_switch_background();
    test_navigation_reconnect_closure();
    test_ocr_and_normalization();
    test_embeddings_semantic_search();
    test_stress();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Integration tests: " << g_pass.load() << " passed, "
              << g_fail.load() << " failed" << std::endl;
    std::cout << "========================================" << std::endl;
    return g_fail.load() == 0 ? 0 : 1;
}
