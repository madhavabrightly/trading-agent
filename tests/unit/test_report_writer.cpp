#include "edge/PerTargetReport.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>

using namespace edgemon;

void test_report_structure() {
    std::cout << "Testing PerTargetReport folder structure..." << std::endl;

    PerTargetReport report;
    TargetId tid("t-report-1");
    std::string ruleUrl = "https://example.com/watch";
    bool ok = report.open(tid, "cdp-abc", ruleUrl, "Example Watch",
                          TargetMatchRule::Exact, "");
    assert(ok);

    std::error_code ec;
    assert(std::filesystem::exists(report.rootPath() + "/metadata.json", ec));
    assert(std::filesystem::exists(report.rootPath() + "/events.jsonl", ec));
    assert(std::filesystem::is_directory(report.rootPath() + "/screenshots", ec));
    assert(std::filesystem::is_directory(report.rootPath() + "/ocr", ec));
    assert(std::filesystem::is_directory(report.rootPath() + "/text", ec));

    // metadata.json content
    std::ifstream meta(report.rootPath() + "/metadata.json");
    std::string metaContent((std::istreambuf_iterator<char>(meta)),
                            std::istreambuf_iterator<char>());
    assert(metaContent.find("t-report-1") != std::string::npos);
    assert(metaContent.find(ruleUrl) != std::string::npos);
    assert(metaContent.find("credentials") == std::string::npos);

    std::cout << "  structure: PASS" << std::endl;
}

void test_ocr_cycle_writes() {
    std::cout << "Testing OcrCycleRecord write..." << std::endl;

    PerTargetReport report;
    TargetId tid("t-report-2");
    assert(report.open(tid, "cdp-def", "https://example.com/a", "Page A",
                       TargetMatchRule::OriginPath, ""));

    OcrCycleRecord cycle;
    cycle.timestamp = "2026-09-02 21:55:01";
    cycle.targetId = tid.value;
    cycle.cdpTargetId = "cdp-def";
    cycle.url = "https://example.com/a";
    cycle.title = "Page A";
    cycle.accepted = true;
    cycle.latencyMs = 521;
    cycle.confidence = 0.91f;
    cycle.text = "Hello world\nSecond line";
    OcrCycleRecord::Line l1;
    l1.text = "Hello world";
    l1.confidence = 0.91f;
    l1.x = 10; l1.y = 20; l1.width = 300; l1.height = 30;
    cycle.lines.push_back(l1);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::string path = report.saveOcrCycle(cycle, &png);

    assert(!path.empty());
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    assert(content.find("\"target_id\": \"t-report-2\"") != std::string::npos);
    assert(content.find("\"cdp_target_id\": \"cdp-def\"") != std::string::npos);
    assert(content.find("\"latency_ms\": 521") != std::string::npos);
    assert(content.find("\"confidence\": 0.91") != std::string::npos);
    assert(content.find("[10.0, 20.0, 300.0, 30.0]") != std::string::npos);
    assert(content.find("Hello world") != std::string::npos);

    // screenshot + text only for changed (non-unchanged) cycles
    std::string stamp = timestampForFile();
    (void)stamp;
    std::error_code ec;
    bool foundScreenshot = false;
    for (auto& f : std::filesystem::directory_iterator(report.rootPath() + "/screenshots", ec)) {
        foundScreenshot = true;
    }
    assert(foundScreenshot);

    std::cout << "  ocr cycle: PASS" << std::endl;
}

void test_unchanged_cycle() {
    std::cout << "Testing unchanged (SKIP_OCR_UNCHANGED) cycle..." << std::endl;

    PerTargetReport report;
    TargetId tid("t-report-3");
    assert(report.open(tid, "cdp-ghi", "https://example.com/static", "Static",
                       TargetMatchRule::Exact, ""));

    OcrCycleRecord cycle;
    cycle.targetId = tid.value;
    cycle.cdpTargetId = "cdp-ghi";
    cycle.url = "https://example.com/static";
    cycle.title = "Static";
    cycle.unchanged = true;

    std::string path = report.saveOcrCycle(cycle, nullptr);
    assert(!path.empty());
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    assert(content.find("\"unchanged\": true") != std::string::npos);

    // No screenshot should have been written for an unchanged cycle.
    std::error_code ec;
    size_t shots = 0;
    for (auto& f : std::filesystem::directory_iterator(report.rootPath() + "/screenshots", ec)) {
        (void)f;
        shots++;
    }
    assert(shots == 0);

    std::cout << "  unchanged: PASS" << std::endl;
}

int main() {
    test_report_structure();
    test_ocr_cycle_writes();
    test_unchanged_cycle();
    std::cout << "All report writer tests passed." << std::endl;
    return 0;
}
