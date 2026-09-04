#include "edge/PerTargetReport.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <algorithm>

namespace edgemon {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::string timestampForFile(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string isoTimestamp(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

PerTargetReport::~PerTargetReport() {
    // Destructor must not silently skip the aggregate when the owner forgets
    // to call writeFinalReport (e.g. Ctrl+C). Only writes when cycles happened.
    if (opened_ && (changedCycles_.load() > 0 || unchangedCycles_.load() > 0)) {
        std::error_code ec;
        std::filesystem::path finalPath(rootPath_ + "/final-report.md");
        if (!std::filesystem::exists(finalPath, ec)) {
            writeFinalReport("");
        }
    }
}

std::string PerTargetReport::safeDirName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "target";
    if (out.size() > 80) out = out.substr(0, 80);
    return out;
}

void PerTargetReport::ensureDir(const std::string& sub) {
    std::error_code ec;
    std::filesystem::create_directories(rootPath_ + "/" + sub, ec);
}

bool PerTargetReport::open(const TargetId& targetId, const std::string& cdpTargetId,
                           const std::string& url, const std::string& title,
                           TargetMatchRule rule, const std::string& profileHint) {
    targetId_ = targetId;
    metadataCdp_ = cdpTargetId;
    metadataUrl_ = url;
    metadataTitle_ = title;
    metadataRule_ = rule;
    metadataProfile_ = profileHint;

    rootPath_ = "reports/" + safeDirName(targetId.value);
    std::error_code ec;
    std::filesystem::create_directories(rootPath_, ec);
    if (ec) {
        LOG_ERROR("PerTargetReport: cannot create {}: {}", rootPath_, ec.message());
        return false;
    }
    ensureDir("screenshots");
    ensureDir("ocr");
    ensureDir("text");
    opened_ = true;

    nlohmann::json meta;
    meta["target_id"] = targetId.value;
    meta["cdp_target_id"] = cdpTargetId;
    meta["url"] = url;
    meta["title"] = title;
    meta["target_rule"] = TargetMatchRuleToString(rule);
    meta["profile_hint"] = profileHint;   // a path string, never credentials
    meta["created"] = isoTimestamp();
    meta["note"] = "No cookies, credentials, or session data are ever stored.";

    std::ofstream out(rootPath_ + "/metadata.json", std::ios::binary);
    if (out) {
        out << meta.dump(2);
        out.close();
    }

    // events.jsonl gets a header event too.
    appendEvent("MONITOR_STARTED", "target=" + targetId.value + " url=" + url);
    LOG_INFO("PerTargetReport: opened {}", rootPath_);
    return true;
}

void PerTargetReport::appendEvent(const std::string& event, const std::string& detail) {
    if (!opened_) return;
    nlohmann::json ev;
    ev["timestamp"] = isoTimestamp();
    ev["event"] = event;
    ev["target_id"] = targetId_.value;
    ev["detail"] = detail;

    std::ofstream out(rootPath_ + "/events.jsonl", std::ios::app | std::ios::binary);
    if (out) {
        out << ev.dump() << "\n";
        out.close();
    }
}

std::string PerTargetReport::saveOcrCycle(const OcrCycleRecord& cycle,
                                          const std::vector<uint8_t>* screenshotPng) {
    if (!opened_) return "";

    if (cycle.unchanged) {
        unchangedCycles_++;
    } else {
        changedCycles_++;
    }

    nlohmann::json j;
    j["timestamp"] = cycle.timestamp.empty() ? isoTimestamp() : cycle.timestamp;
    j["target_id"] = targetId_.value;
    j["cdp_target_id"] = cycle.cdpTargetId.empty() ? metadataCdp_ : cycle.cdpTargetId;
    j["url"] = cycle.url.empty() ? metadataUrl_ : cycle.url;
    j["title"] = cycle.title.empty() ? metadataTitle_ : cycle.title;
    j["latency_ms"] = cycle.latencyMs;
    j["unchanged"] = cycle.unchanged;
    j["accepted"] = cycle.accepted;
    j["rejected"] = cycle.rejected;
    if (!cycle.rejectionReason.empty()) j["rejection_reason"] = cycle.rejectionReason;
    j["confidence"] = cycle.confidence;

    nlohmann::json linesJson = nlohmann::json::array();
    for (const auto& l : cycle.lines) {
        nlohmann::json line;
        line["text"] = l.text;
        line["confidence"] = l.confidence;
        line["bbox"] = {l.x, l.y, l.width, l.height};
        linesJson.push_back(std::move(line));
    }
    j["lines"] = std::move(linesJson);

    // Keep the plain text at top level for quick grepping.
    if (!cycle.text.empty()) j["text"] = cycle.text;

    std::string fileStamp = timestampForFile();
    std::string path = rootPath_ + "/ocr/" + fileStamp + ".json";
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            LOG_ERROR("PerTargetReport: cannot write {}", path);
            return "";
        }
        out << j.dump(2);
        out.close();
    }

    if (!cycle.unchanged) {
        // Screenshot only for CHANGED frames (bounded disk on live pages).
        if (screenshotPng && !screenshotPng->empty()) {
            std::ofstream png(rootPath_ + "/screenshots/" + fileStamp + ".png",
                              std::ios::binary);
            if (png) {
                png.write(reinterpret_cast<const char*>(screenshotPng->data()),
                          static_cast<std::streamsize>(screenshotPng->size()));
                png.close();
            }
        }
        if (!cycle.text.empty()) {
            std::ofstream txt(rootPath_ + "/text/" + fileStamp + ".txt", std::ios::binary);
            if (txt) {
                txt << cycle.text;
                txt.close();
            }
        }
    }

    if (cycle.accepted) ocrAccepted_++;
    if (cycle.rejected) ocrRejected_++;

    LOG_DEBUG("PerTargetReport: wrote OCR cycle {} (unchanged={})", path, cycle.unchanged);
    return path;
}

void PerTargetReport::writeFinalReport(const std::string& extraSummary) {
    if (!opened_) return;

    std::ostringstream md;
    md << "# Edge Monitor — Exact-Target Report\n\n";
    md << "- **Target ID**: " << targetId_.value << "\n";
    md << "- **CDP target ID**: " << metadataCdp_ << "\n";
    md << "- **URL**: " << metadataUrl_ << "\n";
    md << "- **Title**: " << metadataTitle_ << "\n";
    md << "- **Match rule**: " << TargetMatchRuleToString(metadataRule_) << "\n";
    md << "- **Profile hint**: " << (metadataProfile_.empty() ? "(unknown)" : metadataProfile_) << "\n";
    md << "- **Report dir**: " << rootPath_ << "\n\n";

    auto now = std::chrono::steady_clock::now();
    auto elapsedMin = std::chrono::duration<double, std::milli>(now - started_).count() / 60000.0;
    md << "## Summary\n\n";
    md << "- Changed cycles (OCR ran): " << changedCycles_.load() << "\n";
    md << "- Unchanged cycles (SKIP_OCR_UNCHANGED): " << unchangedCycles_.load() << "\n";
    md << "- OCR accepted: " << ocrAccepted_.load() << "\n";
    md << "- OCR rejected: " << ocrRejected_.load() << "\n";
    md << "- Monitoring duration (min): " << std::fixed << std::setprecision(2)
       << elapsedMin << "\n\n";
    md << "## Authentication note\n\n";
    md << "This report contains page OCR text and screenshots only. No cookies, "
          "credentials, or session data are stored.\n";
    if (!extraSummary.empty()) {
        md << "\n" << extraSummary << "\n";
    }

    std::ofstream out(rootPath_ + "/final-report.md", std::ios::binary);
    if (out) {
        out << md.str();
        out.close();
    }
    LOG_INFO("PerTargetReport: wrote {}", rootPath_ + "/final-report.md");
}

} // namespace edgemon
