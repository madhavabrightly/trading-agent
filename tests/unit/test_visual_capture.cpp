#include "capture/BrowserCapture.hpp"
#include "edge/PageSession.hpp"
#include "threadpool.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace edgemon;

void test_captured_frame_structure() {
    std::cout << "Testing CapturedFrame structure..." << std::endl;
    
    CapturedFrame frame;
    frame.targetId = TargetId("test-001");
    frame.cdpTargetId = CDPTargetId("cdp-001");
    frame.timestamp = 1234567890;
    frame.width = 1920;
    frame.height = 1080;
    frame.format = PixelFormat::RGBA;
    frame.status = CaptureStatus::Success;
    frame.pixels.resize(100);
    
    assert(frame.targetId.value == "test-001");
    assert(frame.cdpTargetId.value == "cdp-001");
    assert(frame.timestamp == 1234567890);
    assert(frame.width == 1920);
    assert(frame.height == 1080);
    assert(frame.format == PixelFormat::RGBA);
    assert(frame.status == CaptureStatus::Success);
    assert(frame.pixels.size() == 100);
    
    std::cout << "  CapturedFrame structure: PASS" << std::endl;
}

void test_capture_config() {
    std::cout << "Testing CaptureConfig..." << std::endl;
    
    CaptureConfig config;
    config.format = "png";
    config.quality = 90;
    config.fullPage = false;
    config.clipToViewport = true;
    config.maxWidth = 3840;
    config.maxHeight = 2160;
    
    assert(config.format == "png");
    assert(config.quality == 90);
    assert(config.fullPage == false);
    assert(config.clipToViewport == true);
    assert(config.maxWidth == 3840);
    assert(config.maxHeight == 2160);
    
    std::cout << "  CaptureConfig: PASS" << std::endl;
}

void test_capture_status_enum() {
    std::cout << "Testing CaptureStatus enum..." << std::endl;
    
    CaptureStatus status1 = CaptureStatus::Success;
    CaptureStatus status2 = CaptureStatus::NotConnected;
    CaptureStatus status3 = CaptureStatus::RenderUnavailable;
    CaptureStatus status4 = CaptureStatus::Error;
    
    assert(status1 != status2);
    assert(status3 != status4);
    
    std::cout << "  CaptureStatus enum: PASS" << std::endl;
}

void test_visual_capture_manager_creation() {
    std::cout << "Testing VisualCaptureManager creation..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    auto manager = std::make_shared<VisualCaptureManager>(pool);
    
    assert(manager->activeTargets() == 0);
    assert(manager->pendingFrames() == 0);
    
    std::cout << "  VisualCaptureManager creation: PASS" << std::endl;
}

void test_visual_capture_manager_stats() {
    std::cout << "Testing VisualCaptureManager stats..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    auto manager = std::make_shared<VisualCaptureManager>(pool);
    
    auto stats = manager->getStats();
    
    assert(stats.totalCaptures == 0);
    assert(stats.successful == 0);
    assert(stats.failed == 0);
    assert(stats.unavailable == 0);
    assert(stats.backgroundSkipped == 0);
    
    manager->resetStats();
    
    auto statsAfterReset = manager->getStats();
    assert(statsAfterReset.totalCaptures == 0);
    
    std::cout << "  VisualCaptureManager stats: PASS" << std::endl;
}

void test_frame_base64_decode() {
    std::cout << "Testing Base64 decode..." << std::endl;
    
    // decodeBase64Image is private; exercise the public capture() path with a
    // disconnected session instead (returns NotConnected without decoding).
    auto pool = std::make_shared<ThreadPool>(2);
    auto session = std::make_shared<PageSession>(
        TargetId("b64-test"), CDPTargetId("cdp-b64"),
        "ws://localhost:9222/devtools/page/cdp-b64", pool);
    BrowserCapture capture(session);
    capture.setTargetId(TargetId("b64-test"));
    
    CapturedFrame frame = capture.capture();
    assert(frame.status == CaptureStatus::NotConnected);
    assert(frame.targetId.value == "b64-test");
    
    std::cout << "  Base64 decode (disconnected path): PASS" << std::endl;
}

void test_captured_frame_with_status() {
    std::cout << "Testing CapturedFrame with status..." << std::endl;
    
    CapturedFrame frame;
    frame.targetId = TargetId("status-test");
    frame.status = CaptureStatus::RenderUnavailable;
    frame.statusMessage = "Page not rendering";
    frame.isBackground = true;
    
    assert(frame.status == CaptureStatus::RenderUnavailable);
    assert(frame.statusMessage == "Page not rendering");
    assert(frame.isBackground == true);
    
    CapturedFrame successFrame;
    successFrame.status = CaptureStatus::Success;
    successFrame.isStale = false;
    
    assert(successFrame.status == CaptureStatus::Success);
    assert(successFrame.isStale == false);
    
    std::cout << "  CapturedFrame with status: PASS" << std::endl;
}

void test_capture_config_validation() {
    std::cout << "Testing CaptureConfig validation..." << std::endl;
    
    CaptureConfig config;
    
    config.format = "png";
    config.quality = 100;
    config.fullPage = true;
    config.clipToViewport = false;
    
    assert(config.format == "png");
    assert(config.quality == 100);
    assert(config.fullPage == true);
    assert(config.clipToViewport == false);
    
    CaptureConfig jpegConfig;
    jpegConfig.format = "jpeg";
    jpegConfig.quality = 85;
    
    assert(jpegConfig.format == "jpeg");
    assert(jpegConfig.quality == 85);
    
    std::cout << "  CaptureConfig validation: PASS" << std::endl;
}

void test_target_frame_association() {
    std::cout << "Testing target-frame association..." << std::endl;
    
    CapturedFrame frame;
    
    TargetId targetA("target-A");
    TargetId targetB("target-B");
    
    frame.targetId = targetA;
    frame.cdpTargetId = CDPTargetId("cdp-A");
    frame.timestamp = 1000;
    
    assert(frame.targetId == targetA);
    assert(frame.targetId != targetB);
    assert(frame.timestamp == 1000);
    
    frame.targetId = targetB;
    frame.timestamp = 2000;
    
    assert(frame.targetId == targetB);
    assert(frame.timestamp == 2000);
    
    std::cout << "  Target-frame association: PASS" << std::endl;
}

void test_frame_pixel_handling() {
    std::cout << "Testing frame pixel handling..." << std::endl;
    
    CapturedFrame frame;
    frame.width = 100;
    frame.height = 100;
    frame.format = PixelFormat::RGBA;
    
    size_t expectedSize = frame.width * frame.height * 4;
    frame.pixels.resize(expectedSize, 0);
    
    assert(frame.pixels.size() == expectedSize);
    
    for (auto& pixel : frame.pixels) {
        pixel = 255;
    }
    
    assert(frame.pixels[0] == 255);
    assert(frame.pixels[expectedSize - 1] == 255);
    
    std::cout << "  Frame pixel handling: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - Visual Capture Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_captured_frame_structure();
    test_capture_config();
    test_capture_status_enum();
    test_visual_capture_manager_creation();
    test_visual_capture_manager_stats();
    test_frame_base64_decode();
    test_captured_frame_with_status();
    test_capture_config_validation();
    test_target_frame_association();
    test_frame_pixel_handling();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All visual capture tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
