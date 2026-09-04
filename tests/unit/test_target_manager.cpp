#include "core/TargetManager.hpp"
#include "core/TargetRegistry.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace edgemon;

void test_target_registry_singleton() {
    std::cout << "Testing TargetRegistry singleton..." << std::endl;
    
    auto& registry = TargetRegistry::instance();
    registry.clear();
    
    assert(registry.count() == 0);
    
    std::cout << "  TargetRegistry singleton: PASS" << std::endl;
}

void test_target_registry_add_remove() {
    std::cout << "Testing TargetRegistry add/remove..." << std::endl;
    
    auto& registry = TargetRegistry::instance();
    registry.clear();
    
    MonitoredTarget target;
    target.id = TargetId("test-001");
    target.cdpTargetId = CDPTargetId("cdp-001");
    target.edgeProcessId = EdgeProcessId(12345);
    target.url = PageUrl("https://example.com");
    target.title = "Test Page";
    target.state = TargetState::Discovered;
    
    assert(registry.add(target));
    assert(registry.count() == 1);
    assert(registry.exists(TargetId("test-001")));
    
    auto retrieved = registry.get(TargetId("test-001"));
    assert(retrieved.has_value());
    assert(retrieved->url.value == "https://example.com");
    
    assert(registry.remove(TargetId("test-001")));
    assert(registry.count() == 0);
    
    std::cout << "  TargetRegistry add/remove: PASS" << std::endl;
}

void test_target_registry_state_changes() {
    std::cout << "Testing TargetRegistry state changes..." << std::endl;
    
    auto& registry = TargetRegistry::instance();
    registry.clear();
    
    MonitoredTarget target;
    target.id = TargetId("state-test");
    target.state = TargetState::Discovered;
    registry.add(target);
    
    assert(registry.setState(TargetId("state-test"), TargetState::Connecting));
    auto t1 = registry.get(TargetId("state-test"));
    assert(t1->state == TargetState::Connecting);
    
    assert(registry.setState(TargetId("state-test"), TargetState::Connected, "Connected successfully"));
    auto t2 = registry.get(TargetId("state-test"));
    assert(t2->state == TargetState::Connected);
    assert(t2->stateMessage == "Connected successfully");
    
    assert(registry.setState(TargetId("state-test"), TargetState::Offline, "Target lost"));
    auto t3 = registry.get(TargetId("state-test"));
    assert(t3->state == TargetState::Offline);
    
    registry.remove(TargetId("state-test"));
    
    std::cout << "  TargetRegistry state changes: PASS" << std::endl;
}

void test_target_registry_enabled() {
    std::cout << "Testing TargetRegistry enabled..." << std::endl;
    
    auto& registry = TargetRegistry::instance();
    registry.clear();
    
    MonitoredTarget target;
    target.id = TargetId("enabled-test");
    target.enabled = true;
    registry.add(target);
    
    assert(registry.get(TargetId("enabled-test"))->enabled);
    
    registry.setEnabled(TargetId("enabled-test"), false);
    assert(!registry.get(TargetId("enabled-test"))->enabled);
    
    registry.setEnabled(TargetId("enabled-test"), true);
    assert(registry.get(TargetId("enabled-test"))->enabled);
    
    registry.remove(TargetId("enabled-test"));
    
    std::cout << "  TargetRegistry enabled: PASS" << std::endl;
}

void test_target_registry_get_enabled() {
    std::cout << "Testing TargetRegistry getEnabled..." << std::endl;
    
    auto& registry = TargetRegistry::instance();
    registry.clear();
    
    MonitoredTarget t1;
    t1.id = TargetId("t1");
    t1.enabled = true;
    registry.add(t1);
    
    MonitoredTarget t2;
    t2.id = TargetId("t2");
    t2.enabled = false;
    registry.add(t2);
    
    MonitoredTarget t3;
    t3.id = TargetId("t3");
    t3.enabled = true;
    registry.add(t3);
    
    auto enabled = registry.getEnabled();
    assert(enabled.size() == 2);
    
    registry.clear();
    
    std::cout << "  TargetRegistry getEnabled: PASS" << std::endl;
}

void test_target_registry_get_by_state() {
    std::cout << "Testing TargetRegistry getByState..." << std::endl;
    
    auto& registry = TargetRegistry::instance();
    registry.clear();
    
    MonitoredTarget t1;
    t1.id = TargetId("st1");
    t1.state = TargetState::Connected;
    registry.add(t1);
    
    MonitoredTarget t2;
    t2.id = TargetId("st2");
    t2.state = TargetState::Connected;
    registry.add(t2);
    
    MonitoredTarget t3;
    t3.id = TargetId("st3");
    t3.state = TargetState::Offline;
    registry.add(t3);
    
    auto online = registry.getByState(TargetState::Connected);
    assert(online.size() == 2);
    
    auto offline = registry.getByState(TargetState::Offline);
    assert(offline.size() == 1);
    
    auto error = registry.getByState(TargetState::Error);
    assert(error.empty());
    
    registry.clear();
    
    std::cout << "  TargetRegistry getByState: PASS" << std::endl;
}

void test_target_manager_init() {
    std::cout << "Testing TargetManager initialization..." << std::endl;
    
    auto manager = std::make_shared<TargetManager>();
    manager->initialize();
    
    assert(manager->lockedCount() == 0);
    
    manager->shutdown();
    
    std::cout << "  TargetManager initialization: PASS" << std::endl;
}

void test_target_manager_lock_unlock() {
    std::cout << "Testing TargetManager lock/unlock..." << std::endl;
    
    auto manager = std::make_shared<TargetManager>();
    manager->initialize();
    
    EdgeTarget edgeTarget;
    edgeTarget.id = CDPTargetId("cdp-lock-test");
    edgeTarget.type = "page";
    edgeTarget.title = "Test Page";
    edgeTarget.url = PageUrl("https://test.com");
    edgeTarget.processId = EdgeProcessId(12345);
    
    TargetId lockedId = manager->lockTarget(edgeTarget);
    assert(!lockedId.empty());
    assert(lockedId.value == "target-1");
    
    assert(manager->lockedCount() == 1);
    
    auto locked = manager->getLockedTarget(lockedId);
    assert(locked.has_value());
    assert(locked->url.value == "https://test.com");
    
    assert(manager->unlockTarget(lockedId));
    assert(manager->lockedCount() == 0);
    
    manager->shutdown();
    
    std::cout << "  TargetManager lock/unlock: PASS" << std::endl;
}

void test_target_manager_multiple_locks() {
    std::cout << "Testing TargetManager multiple locks..." << std::endl;
    
    auto manager = std::make_shared<TargetManager>();
    manager->initialize();
    
    EdgeTarget targets[] = {
        {CDPTargetId("cdp-001"), "page", "Page A", PageUrl("https://site-a.com"), "", EdgeProcessId(11111)},
        {CDPTargetId("cdp-002"), "page", "Page B", PageUrl("https://site-b.com"), "", EdgeProcessId(11111)},
        {CDPTargetId("cdp-003"), "page", "Page C", PageUrl("https://site-c.com"), "", EdgeProcessId(22222)},
        {CDPTargetId("cdp-004"), "page", "Page D", PageUrl("https://site-d.com"), "", EdgeProcessId(33333)},
    };
    
    std::vector<TargetId> lockedIds;
    for (const auto& target : targets) {
        TargetId id = manager->lockTarget(target);
        assert(!id.empty());
        lockedIds.push_back(id);
    }
    
    assert(manager->lockedCount() == 4);
    
    auto stats = manager->getStats();
    assert(stats.totalLocked == 4);
    
    manager->unlockAllTargets();
    assert(manager->lockedCount() == 0);
    
    manager->shutdown();
    
    std::cout << "  TargetManager multiple locks: PASS" << std::endl;
}

void test_target_manager_offline_state() {
    std::cout << "Testing TargetManager offline state..." << std::endl;
    
    auto manager = std::make_shared<TargetManager>();
    manager->initialize();
    
    EdgeTarget target;
    target.id = CDPTargetId("cdp-offline");
    target.type = "page";
    target.url = PageUrl("https://offline-test.com");
    target.processId = EdgeProcessId(12345);
    
    TargetId id = manager->lockTarget(target);
    
    auto before = manager->getLockedTarget(id);
    assert(before->state == TargetState::Monitoring);
    
    manager->markTargetOffline(id, "Test offline");
    
    auto after = manager->getLockedTarget(id);
    assert(after->state == TargetState::Offline);
    assert(after->stateMessage == "Test offline");
    
    manager->unlockTarget(id);
    manager->shutdown();
    
    std::cout << "  TargetManager offline state: PASS" << std::endl;
}

void test_target_manager_enable_disable() {
    std::cout << "Testing TargetManager enable/disable..." << std::endl;
    
    auto manager = std::make_shared<TargetManager>();
    manager->initialize();
    
    EdgeTarget target;
    target.id = CDPTargetId("cdp-enable");
    target.type = "page";
    target.url = PageUrl("https://enable-test.com");
    target.processId = EdgeProcessId(12345);
    
    TargetId id = manager->lockTarget(target);
    
    assert(manager->disableTarget(id));
    
    auto disabled = manager->getLockedTarget(id);
    assert(!disabled->enabled);
    
    assert(manager->enableTarget(id));
    
    auto enabled = manager->getLockedTarget(id);
    assert(enabled->enabled);
    
    manager->unlockTarget(id);
    manager->shutdown();
    
    std::cout << "  TargetManager enable/disable: PASS" << std::endl;
}

void test_target_manager_no_duplicate_lock() {
    std::cout << "Testing TargetManager no duplicate lock..." << std::endl;
    
    auto manager = std::make_shared<TargetManager>();
    manager->initialize();
    
    EdgeTarget target;
    target.id = CDPTargetId("cdp-unique");
    target.type = "page";
    target.url = PageUrl("https://unique-test.com");
    target.processId = EdgeProcessId(12345);
    
    TargetId id1 = manager->lockTarget(target);
    TargetId id2 = manager->lockTarget(target);
    
    assert(id1 == id2);
    assert(manager->lockedCount() == 1);
    
    manager->unlockTarget(id1);
    manager->shutdown();
    
    std::cout << "  TargetManager no duplicate lock: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - Target Manager Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_target_registry_singleton();
    test_target_registry_add_remove();
    test_target_registry_state_changes();
    test_target_registry_enabled();
    test_target_registry_get_enabled();
    test_target_registry_get_by_state();
    test_target_manager_init();
    test_target_manager_lock_unlock();
    test_target_manager_multiple_locks();
    test_target_manager_offline_state();
    test_target_manager_enable_disable();
    test_target_manager_no_duplicate_lock();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
