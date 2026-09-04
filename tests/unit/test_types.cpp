#include "core/Types.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace edgemon;

void test_target_id() {
    std::cout << "Testing TargetId..." << std::endl;
    
    TargetId id1("target-001");
    TargetId id2("target-002");
    TargetId id3("target-001");
    
    assert(id1.value == "target-001");
    assert(id1 == id3);
    assert(id1 != id2);
    assert(!id1.empty());
    
    TargetId empty;
    assert(empty.empty());
    
    std::cout << "  TargetId: PASS" << std::endl;
}

void test_cdp_target_id() {
    std::cout << "Testing CDPTargetId..." << std::endl;
    
    CDPTargetId id1("cdp-abc123");
    CDPTargetId id2("cdp-def456");
    
    assert(id1.value == "cdp-abc123");
    assert(id1 != id2);
    assert(!id1.empty());
    
    std::cout << "  CDPTargetId: PASS" << std::endl;
}

void test_edge_process_id() {
    std::cout << "Testing EdgeProcessId..." << std::endl;
    
    EdgeProcessId pid1(12345);
    EdgeProcessId pid2(67890);
    
    assert(pid1.value == 12345);
    assert(pid1 != pid2);
    assert(!pid1.empty());
    assert(pid1 < pid2);
    
    EdgeProcessId empty;
    assert(empty.empty());
    
    std::cout << "  EdgeProcessId: PASS" << std::endl;
}

void test_page_url() {
    std::cout << "Testing PageUrl..." << std::endl;
    
    PageUrl url1("https://example.com/page1");
    PageUrl url2("https://example.com/page2");
    
    assert(url1.value == "https://example.com/page1");
    assert(url1 != url2);
    assert(!url1.empty());
    assert(url1.contains("example"));
    assert(!url1.contains("notfound"));
    
    std::cout << "  PageUrl: PASS" << std::endl;
}

void test_target_state() {
    std::cout << "Testing TargetState..." << std::endl;
    
    assert(std::string(TargetStateToString(TargetState::Discovered)) == "Discovered");
    assert(std::string(TargetStateToString(TargetState::Connecting)) == "Connecting");
    assert(std::string(TargetStateToString(TargetState::Connected)) == "Connected");
    assert(std::string(TargetStateToString(TargetState::Monitoring)) == "Monitoring");
    assert(std::string(TargetStateToString(TargetState::Reconnecting)) == "Reconnecting");
    assert(std::string(TargetStateToString(TargetState::Offline)) == "Offline");
    assert(std::string(TargetStateToString(TargetState::Error)) == "Error");
    
    std::cout << "  TargetState: PASS" << std::endl;
}

void test_monitored_target() {
    std::cout << "Testing MonitoredTarget..." << std::endl;
    
    MonitoredTarget target;
    target.id = TargetId("test-001");
    target.cdpTargetId = CDPTargetId("cdp-xyz");
    target.edgeProcessId = EdgeProcessId(12345);
    target.url = PageUrl("https://test.com");
    target.title = "Test Page";
    target.state = TargetState::Discovered;
    
    assert(target.id.value == "test-001");
    assert(target.cdpTargetId.value == "cdp-xyz");
    assert(target.edgeProcessId.value == 12345);
    assert(target.url.value == "https://test.com");
    assert(target.title == "Test Page");
    assert(target.state == TargetState::Discovered);
    assert(target.enabled);
    
    std::cout << "  MonitoredTarget: PASS" << std::endl;
}

void test_edge_instance() {
    std::cout << "Testing EdgeInstance..." << std::endl;
    
    EdgeInstance instance;
    instance.processId = EdgeProcessId(54321);
    instance.debuggingPort = 9222;
    instance.browserVersion = "Microsoft Edge 120.0.0.0";
    
    assert(instance.processId.value == 54321);
    assert(instance.debuggingPort == 9222);
    assert(instance.browserVersion == "Microsoft Edge 120.0.0.0");
    assert(!instance.empty());
    
    std::cout << "  EdgeInstance: PASS" << std::endl;
}

void test_edge_target() {
    std::cout << "Testing EdgeTarget..." << std::endl;
    
    EdgeTarget target;
    target.id = CDPTargetId("target-123");
    target.type = "page";
    target.title = "Example Page";
    target.url = PageUrl("https://example.com");
    target.processId = EdgeProcessId(11111);
    
    assert(target.id.value == "target-123");
    assert(target.isPage());
    assert(!target.isBackgroundPage());
    assert(!target.isServiceWorker());
    assert(target.url.value == "https://example.com");
    
    EdgeTarget bgTarget;
    bgTarget.type = "background_page";
    assert(bgTarget.isBackgroundPage());
    
    std::cout << "  EdgeTarget: PASS" << std::endl;
}

void test_monitoring_config() {
    std::cout << "Testing MonitoringConfig..." << std::endl;
    
    MonitoringConfig config;
    config.domEnabled = true;
    config.visualEnabled = true;
    config.ocrEnabled = false;
    config.mode = CaptureMode::Hybrid;
    config.policy.domPollRateHz = 2.0;
    config.policy.visualCaptureRateHz = 1.0;
    
    assert(config.domEnabled);
    assert(config.visualEnabled);
    assert(!config.ocrEnabled);
    assert(config.mode == CaptureMode::Hybrid);
    assert(config.policy.domPollRateHz == 2.0);
    
    std::cout << "  MonitoringConfig: PASS" << std::endl;
}

void test_observation() {
    std::cout << "Testing Observation..." << std::endl;
    
    Observation obs;
    obs.targetId = TargetId("obs-001");
    obs.timestamp = 1234567890;
    obs.source = ContentSource::DOM;
    obs.rawText = "BTC: $104,250";
    obs.normalizedText = "BTC: 104250";
    obs.confidence = 0.95f;
    obs.contentHash = 987654321;
    
    assert(obs.targetId.value == "obs-001");
    assert(obs.timestamp == 1234567890);
    assert(obs.source == ContentSource::DOM);
    assert(obs.rawText == "BTC: $104,250");
    assert(obs.confidence == 0.95f);
    
    Observation::BoundingBox box;
    box.x = 100;
    box.y = 200;
    box.width = 300;
    box.height = 50;
    obs.boxes.push_back(box);
    assert(obs.boxes.size() == 1);
    
    std::cout << "  Observation: PASS" << std::endl;
}

void test_embedding() {
    std::cout << "Testing Embedding..." << std::endl;
    
    Embedding emb;
    emb.id = "emb-001";
    emb.targetId = TargetId("target-001");
    emb.content = "Bitcoin price update";
    emb.vector = {0.1f, 0.2f, 0.3f, 0.4f};
    emb.timestamp = 1234567890;
    emb.source = ContentSource::OCR;
    emb.confidence = 0.92f;
    
    assert(emb.id == "emb-001");
    assert(emb.vector.size() == 4);
    assert(emb.source == ContentSource::OCR);
    
    std::cout << "  Embedding: PASS" << std::endl;
}

void test_frame() {
    std::cout << "Testing Frame..." << std::endl;
    
    Frame frame;
    frame.targetId = TargetId("frame-001");
    frame.cdpTargetId = CDPTargetId("cdp-frame");
    frame.timestamp = 1234567890;
    frame.width = 1920;
    frame.height = 1080;
    frame.format = PixelFormat::BGRA;
    frame.available = true;
    
    auto pixels = std::make_shared<PixelBuffer>();
    pixels->width = 1920;
    pixels->height = 1080;
    pixels->format = PixelFormat::BGRA;
    pixels->data.resize(1920 * 1080 * 4);
    frame.pixels = pixels;
    
    assert(frame.targetId.value == "frame-001");
    assert(frame.width == 1920);
    assert(frame.height == 1080);
    assert(frame.format == PixelFormat::BGRA);
    assert(frame.available);
    assert(frame.pixels != nullptr);
    assert(frame.pixels->size() == 1920 * 1080 * 4);
    
    std::cout << "  Frame: PASS" << std::endl;
}

void test_hash_specializations() {
    std::cout << "Testing hash specializations..." << std::endl;
    
    std::hash<TargetId> targetIdHash;
    std::hash<CDPTargetId> cdpTargetIdHash;
    std::hash<EdgeProcessId> processIdHash;
    
    TargetId t1("test-1");
    TargetId t2("test-2");
    
    assert(targetIdHash(t1) != targetIdHash(t2));
    
    EdgeProcessId p1(100);
    EdgeProcessId p2(200);
    
    assert(processIdHash(p1) != processIdHash(p2));
    
    std::cout << "  Hash specializations: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - Type System Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_target_id();
    test_cdp_target_id();
    test_edge_process_id();
    test_page_url();
    test_target_state();
    test_monitored_target();
    test_edge_instance();
    test_edge_target();
    test_monitoring_config();
    test_observation();
    test_embedding();
    test_frame();
    test_hash_specializations();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
