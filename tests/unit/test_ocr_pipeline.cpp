#include "extraction/OCRManager.hpp"
#include "capture/ChangeDetector.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace edgemon;

void test_ocr_result_structure() {
    std::cout << "Testing OCRResult structure..." << std::endl;
    
    OCRResult result;
    result.targetId = TargetId("test-001");
    result.text = "BTC: $104,250";
    result.confidence = 0.95f;
    result.timestamp = 1234567890;
    result.source = OCRResult::Source::Visual;
    
    result.boxes.resize(1);
    result.boxes[0].x = 100;
    result.boxes[0].y = 200;
    result.boxes[0].width = 300;
    result.boxes[0].height = 50;
    
    assert(result.targetId.value == "test-001");
    assert(result.text == "BTC: $104,250");
    assert(result.confidence == 0.95f);
    assert(result.source == OCRResult::Source::Visual);
    assert(result.boxes.size() == 1);
    assert(result.boxes[0].x == 100);
    
    std::cout << "  OCRResult structure: PASS" << std::endl;
}

void test_ocr_result_rejection() {
    std::cout << "Testing OCRResult rejection..." << std::endl;
    
    OCRResult result;
    result.rejected = false;
    result.confidence = 0.95f;
    
    assert(!result.rejected);
    
    result.rejected = true;
    result.rejectionReason = "Below confidence threshold";
    result.confidence = 0.5f;
    
    assert(result.rejected);
    assert(result.rejectionReason == "Below confidence threshold");
    
    std::cout << "  OCRResult rejection: PASS" << std::endl;
}

void test_ocr_config() {
    std::cout << "Testing OCRConfig..." << std::endl;
    
    OCRConfig config;
    config.confidenceThreshold = 0.8f;
    config.language = "en";
    config.enableCorrection = true;
    config.preserveLayout = true;
    config.maxTextLength = 5000;
    
    assert(config.confidenceThreshold == 0.8f);
    assert(config.language == "en");
    assert(config.enableCorrection == true);
    assert(config.preserveLayout == true);
    assert(config.maxTextLength == 5000);
    
    std::cout << "  OCRConfig: PASS" << std::endl;
}

void test_ocr_manager_creation() {
    std::cout << "Testing OCRManager creation..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    auto manager = std::make_shared<OCRManager>(pool);
    
    assert(!manager->isInitialized());
    
    std::cout << "  OCRManager creation: PASS" << std::endl;
}

void test_ocr_manager_init() {
    std::cout << "Testing OCRManager initialization..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    auto manager = std::make_shared<OCRManager>(pool);
    
    manager->initialize();
    assert(manager->isInitialized());
    
    manager->shutdown();
    
    std::cout << "  OCRManager initialization: PASS" << std::endl;
}

void test_ocr_manager_config() {
    std::cout << "Testing OCRManager config..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    auto manager = std::make_shared<OCRManager>(pool);
    
    OCRConfig config;
    config.confidenceThreshold = 0.9f;
    config.language = "en";
    
    manager->setConfig(config);
    
    auto retrieved = manager->getConfig();
    assert(retrieved.confidenceThreshold == 0.9f);
    assert(retrieved.language == "en");
    
    std::cout << "  OCRManager config: PASS" << std::endl;
}

void test_frame_change_detector_basic() {
    std::cout << "Testing FrameChangeDetector basic..." << std::endl;
    
    FrameChangeDetector detector;
    
    CapturedFrame frame1;
    frame1.targetId = TargetId("test-001");
    frame1.width = 100;
    frame1.height = 100;
    frame1.pixels.resize(100 * 100 * 4, 0);
    frame1.status = CaptureStatus::Success;
    
    auto result1 = detector.detect(frame1);
    assert(result1.hasChanged == true);
    
    auto result2 = detector.detect(frame1);
    assert(result2.hasChanged == false);
    
    std::cout << "  FrameChangeDetector basic: PASS" << std::endl;
}

void test_frame_change_detector_config() {
    std::cout << "Testing FrameChangeDetector config..." << std::endl;
    
    FrameChangeDetector::Config config;
    config.similarityThreshold = 0.9f;
    config.regionSize = 32;
    config.minChangedPixels = 50;
    config.enableRegionDetection = true;
    
    FrameChangeDetector detector(config);
    
    auto retrieved = detector.getConfig();
    assert(retrieved.similarityThreshold == 0.9f);
    assert(retrieved.regionSize == 32);
    assert(retrieved.minChangedPixels == 50);
    assert(retrieved.enableRegionDetection == true);
    
    std::cout << "  FrameChangeDetector config: PASS" << std::endl;
}

void test_frame_change_detector_stats() {
    std::cout << "Testing FrameChangeDetector stats..." << std::endl;
    
    FrameChangeDetector detector;
    
    CapturedFrame frame;
    frame.targetId = TargetId("test-001");
    frame.width = 100;
    frame.height = 100;
    frame.pixels.resize(100 * 100 * 4, 255);
    frame.status = CaptureStatus::Success;
    
    detector.detect(frame);
    detector.detect(frame);
    detector.detect(frame);
    
    auto stats = detector.getStats();
    assert(stats.framesProcessed == 3);
    assert(stats.framesChanged == 1);
    assert(stats.framesSkipped == 2);
    
    std::cout << "  FrameChangeDetector stats: PASS" << std::endl;
}

void test_region_diff() {
    std::cout << "Testing RegionDiff..." << std::endl;
    
    RegionDiff diff;
    diff.x = 10;
    diff.y = 20;
    diff.width = 100;
    diff.height = 50;
    diff.similarity = 0.95f;
    diff.changedPixels = 100;
    diff.hasChanged = true;
    
    assert(diff.x == 10);
    assert(diff.y == 20);
    assert(diff.width == 100);
    assert(diff.height == 50);
    assert(diff.similarity == 0.95f);
    assert(diff.hasChanged == true);
    
    std::cout << "  RegionDiff: PASS" << std::endl;
}

void test_frame_change_result() {
    std::cout << "Testing FrameChangeResult..." << std::endl;
    
    FrameChangeResult result;
    result.hasChanged = true;
    result.overallSimilarity = 0.85f;
    result.frameHash = 12345;
    result.timestamp = 1234567890;
    
    RegionDiff region;
    region.x = 0;
    region.y = 0;
    region.width = 100;
    region.height = 100;
    region.hasChanged = true;
    result.regions.push_back(region);
    
    assert(result.hasChanged == true);
    assert(result.overallSimilarity == 0.85f);
    assert(result.regions.size() == 1);
    assert(result.regions[0].hasChanged == true);
    
    std::cout << "  FrameChangeResult: PASS" << std::endl;
}

void test_ocr_pipeline_creation() {
    std::cout << "Testing OCRPipeline creation..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    auto pipeline = std::make_shared<OCRPipeline>(pool);
    
    assert(pipeline->pendingFrames() == 0);
    assert(pipeline->pendingOCRJobs() == 0);
    
    std::cout << "  OCRPipeline creation: PASS" << std::endl;
}

void test_ocr_pipeline_frame_add() {
    std::cout << "Testing OCRPipeline frame add..." << std::endl;
    
    auto pool = std::make_shared<ThreadPool>(2);
    auto pipeline = std::make_shared<OCRPipeline>(pool);
    
    CapturedFrame frame;
    frame.targetId = TargetId("test-001");
    frame.width = 100;
    frame.height = 100;
    frame.pixels.resize(100 * 100 * 4, 128);
    frame.status = CaptureStatus::Success;
    
    pipeline->start();
    pipeline->addFrame(frame);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    pipeline->stop();
    
    std::cout << "  OCRPipeline frame add: PASS" << std::endl;
}

void test_change_detection_pipeline() {
    std::cout << "Testing change detection pipeline..." << std::endl;
    
    FrameChangeDetector detector;
    
    CapturedFrame frame1;
    frame1.targetId = TargetId("pipeline-test");
    frame1.width = 200;
    frame1.height = 200;
    frame1.pixels.resize(200 * 200 * 4, 0);
    frame1.status = CaptureStatus::Success;
    
    CapturedFrame frame2;
    frame2.targetId = TargetId("pipeline-test");
    frame2.width = 200;
    frame2.height = 200;
    frame2.pixels.resize(200 * 200 * 4, 255);
    frame2.status = CaptureStatus::Success;
    
    CapturedFrame frame3;
    frame3.targetId = TargetId("pipeline-test");
    frame3.width = 200;
    frame3.height = 200;
    frame3.pixels.resize(200 * 200 * 4, 255);
    frame3.status = CaptureStatus::Success;
    
    auto result1 = detector.detect(frame1);
    assert(result1.hasChanged == true);
    
    auto result2 = detector.detect(frame2);
    assert(result2.hasChanged == true);
    
    auto result3 = detector.detect(frame3);
    assert(result3.hasChanged == false);
    
    std::cout << "  Change detection pipeline: PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Edge Monitor - OCR Pipeline Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_ocr_result_structure();
    test_ocr_result_rejection();
    test_ocr_config();
    test_ocr_manager_creation();
    test_ocr_manager_init();
    test_ocr_manager_config();
    test_frame_change_detector_basic();
    test_frame_change_detector_config();
    test_frame_change_detector_stats();
    test_region_diff();
    test_frame_change_result();
    test_ocr_pipeline_creation();
    test_ocr_pipeline_frame_add();
    test_change_detection_pipeline();
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All OCR pipeline tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
