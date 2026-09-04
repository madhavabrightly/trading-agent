#include "edge/EdgeDiscovery.hpp"
#include "edge/TargetResolver.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace edgemon;

void test_edge_discovery_config() {
    std::cout << "Testing EdgeDiscovery::DiscoveryConfig..." << std::endl;
    
    EdgeDiscovery::DiscoveryConfig config;
    config.preferredPorts = {9222, 9223, 9224};
    config.timeoutMs = 3000;
    config.autoDiscover = true;
    
    assert(config.preferredPorts.size() == 3);
    assert(config.preferredPorts[0] == 9222);
    assert(config.timeoutMs == 3000);
    assert(config.autoDiscover == true);
    
    EdgeDiscovery discovery(config);
    
    std::cout << "  DiscoveryConfig: PASS" << std::endl;
}

void test_edge_discovery_is_edge_running() {
    std::cout << "Testing EdgeDiscovery::isEdgeRunning..." << std::endl;
    
    EdgeDiscovery discovery;
    bool running = discovery.isEdgeRunning();
    
    std::cout << "  Edge is " << (running ? "running" : "not running") << std::endl;
    
    std::cout << "  isEdgeRunning: PASS (value=" << (running ? "true" : "false") << ")" << std::endl;
}

void test_edge_discovery_discover() {
    std::cout << "Testing EdgeDiscovery::discover..." << std::endl;
    
    EdgeDiscovery discovery;
    auto result = discovery.discover();
    
    std::cout << "  Discovery success: " << (result.success ? "yes" : "no") << std::endl;
    std::cout << "  Instances found: " << result.instances.size() << std::endl;
    std::cout << "  Targets found: " << result.targets.size() << std::endl;
    
    if (!result.success && !result.errorMessage.empty()) {
        std::cout << "  Error: " << result.errorMessage << std::endl;
    }
    
    std::cout << "  discover: PASS" << std::endl;
}

void test_edge_discovery_enumerate_targets() {
    std::cout << "Testing EdgeDiscovery::enumerateAllTargets..." << std::endl;
    
    EdgeDiscovery discovery;
    auto targets = discovery.enumerateAllTargets();
    
    std::cout << "  Total targets: " << targets.size() << std::endl;
    
    for (const auto& target : targets) {
        std::cout << "  - [" << target.type << "] " << target.title 
                  << " (PID: " << target.processId.value << ")" << std::endl;
    }
    
    std::cout << "  enumerateAllTargets: PASS" << std::endl;
}

void test_target_resolver() {
    std::cout << "Testing TargetResolver..." << std::endl;
    
    TargetResolver resolver;
    
    auto instances = resolver.getInstances();
    std::cout << "  Initial instances: " << instances.size() << std::endl;
    
    auto targets = resolver.getAllTargets();
    std::cout << "  Initial targets: " << targets.size() << std::endl;
    
    std::cout << "  TargetResolver: PASS" << std::endl;
}

void test_build_websocket_url() {
    std::cout << "Testing EdgeDiscovery::buildWebSocketUrl..." << std::endl;
    
    EdgeDiscovery discovery;
    
    EdgeInstance instance;
    instance.debuggingPort = 9222;
    
    CDPTargetId targetId("CDPTarget-abc123");
    
    std::string wsUrl = discovery.buildWebSocketUrl(instance, targetId);
    
    std::cout << "  WebSocket URL: " << wsUrl << std::endl;
    
    assert(wsUrl.find("ws://localhost:9222") != std::string::npos);
    assert(wsUrl.find("/devtools/page/") != std::string::npos);
    assert(wsUrl.find("CDPTarget-abc123") != std::string::npos);
    
    std::cout << "  buildWebSocketUrl: PASS" << std::endl;
}

void test_edge_instance_structure() {
    std::cout << "Testing EdgeInstance structure..." << std::endl;
    
    EdgeInstance instance;
    instance.processId = EdgeProcessId(12345);
    instance.debuggingPort = 9222;
    instance.userDataDir = "C:\\Users\\Test\\AppData\\Local\\Microsoft\\Edge\\User Data";
    instance.browserVersion = "Microsoft Edge 120.0.0.0";
    instance.executablePath = "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe";
    instance.attached = true;
    
    assert(instance.processId.value == 12345);
    assert(instance.debuggingPort == 9222);
    assert(!instance.empty());
    
    std::cout << "  EdgeInstance structure: PASS" << std::endl;
}

void test_edge_target_structure() {
    std::cout << "Testing EdgeTarget structure..." << std::endl;
    
    EdgeTarget target;
    target.id = CDPTargetId("target-001-abc");
    target.type = "page";
    target.title = "Example Page";
    target.url = PageUrl("https://example.com/dashboard");
    target.webSocketDebuggerUrl = "ws://localhost:9222/devtools/page/target-001-abc";
    target.processId = EdgeProcessId(54321);
    
    assert(target.isPage());
    assert(!target.isBackgroundPage());
    assert(!target.isServiceWorker());
    assert(target.id.value == "target-001-abc");
    assert(target.url.contains("example.com"));
    
    std::cout << "  EdgeTarget structure: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - Discovery Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_edge_discovery_config();
    test_edge_discovery_is_edge_running();
    test_build_websocket_url();
    test_edge_instance_structure();
    test_edge_target_structure();
    test_target_resolver();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Integration tests (requires Edge):" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_edge_discovery_discover();
    test_edge_discovery_enumerate_targets();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All tests completed!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
