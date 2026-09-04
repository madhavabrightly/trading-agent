// Unit tests for TargetRegistry: thread-safe registration, duplicate
// prevention, lookup by CDP id, state tracking, and safe re-registration
// after navigation / CDP reconnect.

#include "core/TargetRegistry.hpp"
#include "core/Types.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

using namespace edgemon;

static MonitoredTarget makeTarget(const std::string& id,
                                  const std::string& cdpId,
                                  uint32_t pid,
                                  const std::string& url,
                                  const std::string& title) {
    MonitoredTarget t;
    t.id = TargetId(id);
    t.cdpTargetId = CDPTargetId(cdpId);
    t.edgeProcessId = EdgeProcessId(pid);
    t.url = PageUrl(url);
    t.title = title;
    t.state = TargetState::Discovered;
    t.enabled = true;
    return t;
}

void test_registry_singleton() {
    std::cout << "Testing TargetRegistry singleton..." << std::endl;
    auto& a = TargetRegistry::instance();
    auto& b = TargetRegistry::instance();
    assert(&a == &b);
    a.clear();
    std::cout << "  singleton: PASS" << std::endl;
}

void test_registry_add_get_remove() {
    std::cout << "Testing add/get/remove..." << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    assert(reg.add(makeTarget("r1", "cdp-r1", 100, "https://r1.example", "R1")));
    assert(reg.count() == 1);
    assert(reg.exists(TargetId("r1")));
    assert(!reg.exists(TargetId("missing")));

    auto got = reg.get(TargetId("r1"));
    assert(got.has_value());
    assert(got->cdpTargetId.value == "cdp-r1");
    assert(got->edgeProcessId.value == 100);
    assert(got->url.value == "https://r1.example");

    assert(reg.remove(TargetId("r1")));
    assert(reg.count() == 0);
    assert(!reg.get(TargetId("r1")).has_value());

    std::cout << "  add/get/remove: PASS" << std::endl;
}

void test_registry_duplicate_prevention() {
    std::cout << "Testing duplicate prevention..." << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    assert(reg.add(makeTarget("d1", "cdp-d1", 100, "https://d1.example", "D1")));

    // Same internal id -> rejected.
    MonitoredTarget dupId = makeTarget("d1", "cdp-other", 100, "https://other.example", "Dup");
    assert(!reg.add(dupId));

    // Same CDP id -> rejected (identity is CDP target, not URL).
    MonitoredTarget dupCdp = makeTarget("d2", "cdp-d1", 100, "https://d2.example", "D2");
    assert(!reg.add(dupCdp));

    // Distinct CDP id, same URL -> allowed (two tabs of the same site).
    assert(reg.add(makeTarget("d3", "cdp-d3", 100, "https://d1.example", "D1 copy")));

    assert(reg.count() == 2);
    assert(reg.getByCDPId(CDPTargetId("cdp-d1")).has_value());
    assert(reg.getByCDPId(CDPTargetId("cdp-d3")).has_value());

    std::cout << "  duplicate prevention: PASS" << std::endl;
}

void test_registry_lookup_by_cdp() {
    std::cout << "Testing lookup by CDP id..." << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    reg.add(makeTarget("l1", "cdp-l1", 200, "https://l1.example", "L1"));
    reg.add(makeTarget("l2", "cdp-l2", 200, "https://l2.example", "L2"));

    auto byCdp = reg.getByCDPId(CDPTargetId("cdp-l2"));
    assert(byCdp.has_value());
    assert(byCdp->id.value == "l2");

    assert(!reg.getByCDPId(CDPTargetId("cdp-nope")).has_value());
    assert(!reg.existsByCDPId(CDPTargetId("cdp-nope")));
    assert(reg.existsByCDPId(CDPTargetId("cdp-l1")));

    auto all = reg.getAll();
    assert(all.size() == 2);

    std::cout << "  lookup by CDP id: PASS" << std::endl;
}

void test_registry_state_and_reenroll() {
    std::cout << "Testing state tracking + re-registration..." << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    reg.add(makeTarget("s1", "cdp-s1", 300, "https://s1.example", "S1"));

    assert(reg.setState(TargetId("s1"), TargetState::Connected));
    assert(reg.setState(TargetId("s1"), TargetState::Monitoring));
    auto got = reg.get(TargetId("s1"));
    assert(got->state == TargetState::Monitoring);

    assert(reg.setEnabled(TargetId("s1"), false));
    assert(!reg.get(TargetId("s1"))->enabled);

    // Re-registration after navigation/reconnect: same internal id,
    // updated CDP id + url.
    assert(reg.setCDPTargetId(TargetId("s1"), CDPTargetId("cdp-s1-v2")));
    got = reg.get(TargetId("s1"));
    assert(got->cdpTargetId.value == "cdp-s1-v2");
    assert(!reg.getByCDPId(CDPTargetId("cdp-s1")).has_value());
    assert(reg.getByCDPId(CDPTargetId("cdp-s1-v2")).has_value());

    reg.updateLastHash(TargetId("s1"), 12345, 67890);
    got = reg.get(TargetId("s1"));
    assert(got->lastDomHash == 12345);
    assert(got->lastVisualHash == 67890);

    std::cout << "  state tracking + re-registration: PASS" << std::endl;
}

void test_registry_thread_safety() {
    std::cout << "Testing thread-safe concurrent access..." << std::endl;
    auto& reg = TargetRegistry::instance();
    reg.clear();

    std::vector<std::thread> threads;
    std::atomic<int> added{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&reg, &added, t]() {
            for (int i = 0; i < 50; ++i) {
                std::string id = "thr" + std::to_string(t) + "_" + std::to_string(i);
                if (reg.add(makeTarget(id, "cdp-" + id, 400 + t, "https://" + id + ".example", id))) {
                    added++;
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    assert(added.load() == 400);
    assert(reg.count() == 400);

    // Concurrent reads + writes must not crash and must stay consistent.
    threads.clear();
    std::atomic<int> found{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&reg, &found, t]() {
            for (int i = 0; i < 50; ++i) {
                std::string id = "thr" + std::to_string(t) + "_" + std::to_string(i);
                if (reg.get(TargetId(id)).has_value()) found++;
            }
        });
    }
    for (auto& th : threads) th.join();
    assert(found.load() == 200);

    reg.clear();
    std::cout << "  thread safety: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - TargetRegistry Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_registry_singleton();
    test_registry_add_get_remove();
    test_registry_duplicate_prevention();
    test_registry_lookup_by_cdp();
    test_registry_state_and_reenroll();
    test_registry_thread_safety();

    std::cout << "\nAll registry unit tests passed." << std::endl;
    return 0;
}
