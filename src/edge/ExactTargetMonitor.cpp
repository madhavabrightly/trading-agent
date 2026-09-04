#include "edge/ExactTargetMonitor.hpp"
#include "edge/UrlUtils.hpp"
#include "edge/EdgeConnector.hpp"
#include "edge/EdgeDiscovery.hpp"
#include "extraction/OcrEngine.hpp"
#include "logger.hpp"

#include <sstream>
#include <algorithm>
#include <array>

namespace edgemon {

namespace {

// Minimal base64 decode (same algorithm as BrowserCapture / main.cpp). CDP
// screenshots arrive as base64-encoded PNG bytes.
std::vector<uint8_t> decodeBase64(const std::string& in) {
    std::vector<uint8_t> decoded;
    if (in.empty()) return decoded;
    static const char* chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> lookup{};
    lookup.fill(-1);
    for (int i = 0; i < 64; i++) lookup[static_cast<unsigned char>(chars[i])] = i;

    std::array<uint8_t, 4> buf{};
    size_t bufPos = 0;
    for (char c : in) {
        if (c == '=') break;
        int v = lookup[static_cast<unsigned char>(c)];
        if (v < 0) continue;
        buf[bufPos++] = static_cast<uint8_t>(v);
        if (bufPos == 4) {
            decoded.push_back(static_cast<uint8_t>((buf[0] << 2) | (buf[1] >> 4)));
            decoded.push_back(static_cast<uint8_t>((buf[1] << 4) | (buf[2] >> 2)));
            decoded.push_back(static_cast<uint8_t>((buf[2] << 6) | buf[3]));
            bufPos = 0;
        }
    }
    if (bufPos > 0) {
        for (size_t i = bufPos; i < 4; i++) buf[i] = 0;
        decoded.push_back(static_cast<uint8_t>((buf[0] << 2) | (buf[1] >> 4)));
        if (bufPos > 1) {
            decoded.push_back(static_cast<uint8_t>((buf[1] << 4) | (buf[2] >> 2)));
        }
    }
    return decoded;
}

} // namespace

ExactTargetMonitor::ExactTargetMonitor(std::shared_ptr<ThreadPool> pool,
                                       TargetInfo info,
                                       std::shared_ptr<PerTargetReport> report)
    : pool_(std::move(pool))
    , info_(std::move(info))
    , report_(std::move(report))
    , ruleUrl_(info_.url.value) {
    FrameChangeDetector::Config cfg;
    cfg.similarityThreshold = 0.95f;
    cfg.enableRegionDetection = false;   // whole-frame decision is enough here
    changeDetector_ = std::make_unique<FrameChangeDetector>(cfg);
}

ExactTargetMonitor::~ExactTargetMonitor() {
    stop();
}

void ExactTargetMonitor::pushLog(const std::string& line) {
    LOG_INFO("ExactTargetMonitor: {}", line);
    if (logCb_) {
        try {
            logCb_(line);
        } catch (...) {}
    }
}

void ExactTargetMonitor::detach() {
    if (session_) {
        session_->disconnect();
        session_.reset();
    }
    sessionLive_ = false;
}

bool ExactTargetMonitor::attach() {
    if (sessionLive_) return true;
    if (info_.cdpTargetId.empty() || info_.webSocketUrl.empty()) {
        pushLog("attach failed: no CDP target / websocket URL for " +
                info_.internalId.value);
        return false;
    }
    // Drop any previous half-open session before creating the new one.
    if (session_) {
        session_->disconnect();
        session_.reset();
    }
    auto session = std::make_shared<PageSession>(info_.internalId, info_.cdpTargetId,
                                                 info_.webSocketUrl, pool_);
    session->setErrorCallback([this](const std::string& err, bool recoverable) {
        pushLog("session error (recoverable=" + std::string(recoverable ? "yes" : "no") +
                "): " + err);
    });
    if (session->connect()) {
        session->enablePageEvents();
        session_ = session;
        sessionLive_ = true;
        // PageSession::connect() is async: wait until the session reaches
        // Monitoring (websocket + CDP domains fully initialized) before any
        // command is issued, or the first screenshot races initialization.
        bool ready = false;
        for (int i = 0; i < 20; ++i) {
            if (session_->isConnected() &&
                session_->getState() == TargetState::Monitoring) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (!ready) {
            pushLog("attach: session did not reach Monitoring for " +
                    info_.url.value);
            detach();
            return false;
        }
        pushLog("attached to CDP target " + info_.cdpTargetId.value + " url=" +
                info_.url.value);
        return true;
    }
    pushLog("attach failed: PageSession connect() returned false for " +
            info_.url.value);
    return false;
}

void ExactTargetMonitor::refreshPageMetadata() {
    if (!sessionLive_ || !session_) return;
    auto res = session_->executeJavaScriptSync(
        R"((function(){ return JSON.stringify({
              title: document.title || '',
              url: location.href || ''
            }); })())",
        std::chrono::seconds(4));
    if (!res) return;
    try {
        if (res->contains("result") && (*res)["result"].contains("value") &&
            (*res)["result"]["value"].is_string()) {
            auto parsed = nlohmann::json::parse((*res)["result"]["value"].get<std::string>());
            std::lock_guard<std::mutex> lock(infoMutex_);
            if (parsed.contains("title") && parsed["title"].is_string()) {
                info_.title = parsed["title"].get<std::string>();
            }
            if (parsed.contains("url") && parsed["url"].is_string()) {
                std::string live = parsed["url"].get<std::string>();
                if (!live.empty()) info_.url = PageUrl(live);
            }
        }
    } catch (const std::exception&) {
    }
}

std::string ExactTargetMonitor::currentCdpTargetId() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return info_.cdpTargetId.value;
}

std::string ExactTargetMonitor::currentUrl() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return info_.url.value;
}

std::string ExactTargetMonitor::currentTitle() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return info_.title;
}

std::string ExactTargetMonitor::currentRuleUrl() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return ruleUrl_;
}

std::string ExactTargetMonitor::reattachAfterRestart() {
    // CDP target ids are ephemeral: resolve the URL RULE again, never reuse a
    // stale id.
    detach();

    EdgeProbeResult probe = EdgeConnector::probe();
    if (!probe.cdpAvailable()) {
        pushLog("reattach: no CDP endpoint after restart");
        return "";
    }
    LOG_INFO("ExactTargetMonitor: CDP reconnect -> /json/list on port {}",
             probe.cdpPort);

    // Discover instances/targets via the probe port and match the rule.
    auto discovery = std::make_shared<EdgeDiscovery>();
    EdgeDiscovery::DiscoveryConfig cfg;
    cfg.preferredPorts = {probe.cdpPort};
    discovery->setConfig(cfg);

    auto instances = discovery->discoverAllInstances();
    for (const auto& inst : instances) {
        auto targets = discovery->enumerateTargets(inst);
        for (const auto& t : targets) {
            if (t.type != "page") continue;
            // Re-resolve against the durable URL RULE, never the live URL: an
            // in-page navigation must not let the monitor drift to another tab.
            if (!UrlUtils::matches(ruleUrl_, t.url.value, info_.rule)) continue;
            {
                std::lock_guard<std::mutex> lock(infoMutex_);
                info_.cdpTargetId = t.id;
                info_.webSocketUrl = t.webSocketDebuggerUrl;
                info_.url = PageUrl(t.url.value);
                info_.title = t.title;
            }
            pushLog("reattached: new CDP target " + t.id.value + " url=" +
                    t.url.value);
            attach();
            return t.id.value;
        }
    }
    pushLog("TARGET_NOT_FOUND after restart: no open page matches rule '" +
            ruleUrl_ + "'");
    report_->appendEvent("TARGET_NOT_FOUND",
                         "no open page matches rule '" + ruleUrl_ + "'");
    return "";
}

bool ExactTargetMonitor::start() {
    if (running_.exchange(true)) return false;
    if (!report_) return false;

    report_->appendEvent("TARGET_FOUND", "cdp=" + info_.cdpTargetId.value +
                                            " url=" + info_.url.value);
    pushLog("TARGET LOCKED: " + info_.url.value + " (CDP " +
            info_.cdpTargetId.value + ")");
    pushLog("start: exact-target monitoring of " + info_.url.value);
    thread_ = std::jthread([this](std::stop_token token) { loop(token); });
    return true;
}

void ExactTargetMonitor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    detach();
    if (report_) {
        report_->appendEvent("MONITOR_STOPPED", "cycles=" + std::to_string(cycle_.load()));
        report_->writeFinalReport("");
    }
    pushLog("stopped");
}

void ExactTargetMonitor::loop(std::stop_token token) {
    // Load the real OCR engine once for this monitor (PP-OCRv3 via ONNX).
    OcrEngine ocr;
    bool engineOk = ocr.loadModels("models");
    if (!engineOk) {
        pushLog("OCR engine unavailable: " + ocr.lastError() +
                " — no OCR will run until models are present");
    }

    attach();

    while (!token.stop_requested() && running_) {
        cycle_++;

        // Periodically refresh the aggregate report so a hard kill (Ctrl+C in
        // a console / Stop-Process) never loses the summary — durability must
        // not depend on a graceful destructor.
        if (cycle_.load() % 30 == 0) {
            report_->writeFinalReport("");
        }

        // Session died (websocket closed / Edge restart)? Re-resolve + reattach.
        // sessionLive_ is only set after a successful Monitoring handshake, so
        // also confirm the underlying CDP socket is still live each cycle.
        if (sessionLive_ && session_ && !session_->isConnected()) {
            pushLog("session websocket lost — reattaching via URL rule");
            sessionLive_ = false;
        }
        if (!sessionLive_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            EdgeProbeResult pr = EdgeConnector::probe();
            if (pr.cdpAvailable()) {
                reattachAfterRestart();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!sessionLive_) {
                std::this_thread::sleep_for(interval_);
                continue;
            }
        }

        refreshPageMetadata();

        // 1) Capture screenshot from THIS target (PNG bytes).
        auto shot = session_->captureScreenshot("png", 90, false);
        if (shot.empty() || !shot.contains("data")) {
            LOG_DEBUG("ExactTargetMonitor: no screenshot data");
            std::this_thread::sleep_for(interval_);
            continue;
        }
        std::vector<uint8_t> pngBytes;
        try {
            auto b64 = shot["data"].get<std::string>();
            pngBytes = decodeBase64(b64);
        } catch (const std::exception&) {
            std::this_thread::sleep_for(interval_);
            continue;
        }
        if (pngBytes.empty()) {
            std::this_thread::sleep_for(interval_);
            continue;
        }

        // 2) Decode to BGRA once (shared by change detector + OCR).
        std::vector<uint8_t> bgra;
        int imgW = 0, imgH = 0;
        if (!OcrEngine::decodePng(pngBytes, bgra, imgW, imgH) || imgW == 0 || imgH == 0) {
            LOG_DEBUG("ExactTargetMonitor: PNG decode failed");
            std::this_thread::sleep_for(interval_);
            continue;
        }

        CapturedFrame frame;
        frame.targetId = info_.internalId;
        frame.cdpTargetId = info_.cdpTargetId;
        frame.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        frame.width = static_cast<uint32_t>(imgW);
        frame.height = static_cast<uint32_t>(imgH);
        frame.format = PixelFormat::BGRA;
        frame.status = CaptureStatus::Success;
        frame.pixels = std::move(bgra);

        // 3) Change detection (per-target detector). First frame always
        //    "changed"; subsequent identical frames -> SKIP_OCR_UNCHANGED.
        auto change = changeDetector_->detect(frame);
        OcrCycleRecord cycleRec;
        cycleRec.timestamp = isoTimestamp();
        cycleRec.targetId = info_.internalId.value;
        cycleRec.cdpTargetId = info_.cdpTargetId.value;
        cycleRec.url = info_.url.value;
        cycleRec.title = info_.title;

        if (!change.hasChanged) {
            unchangedCycles_++;
            cycleRec.unchanged = true;
            LOG_DEBUG("ExactTargetMonitor: SKIP_OCR_UNCHANGED for {}",
                      info_.internalId.value);
            report_->saveOcrCycle(cycleRec, nullptr);
            std::this_thread::sleep_for(interval_);
            continue;
        }

        changedCycles_++;

        // 4) Real OCR (engine gate; never fabricate text).
        if (!engineOk || !ocr.available()) {
            cycleRec.unchanged = false;
            cycleRec.rejected = true;
            cycleRec.rejectionReason = "OCR engine unavailable";
            report_->saveOcrCycle(cycleRec, nullptr);
            std::this_thread::sleep_for(interval_);
            continue;
        }

        pushLog("OCR_EXECUTION_STARTED (" + std::to_string(imgW) + "x" +
                std::to_string(imgH) + ")");
        OcrPageResult page = ocr.recognize(frame.pixels.data(), imgW, imgH);
        uint64_t latency = static_cast<uint64_t>(ocr.lastLatencyMs());

        if (page.lines.empty()) {
            cycleRec.rejected = true;
            cycleRec.rejectionReason = "No text detected";
            cycleRec.latencyMs = latency;
            report_->saveOcrCycle(cycleRec, &pngBytes);
            pushLog("OCR_EXECUTION_COMPLETED (no text, " + std::to_string(latency) +
                    "ms)");
            std::this_thread::sleep_for(interval_);
            continue;
        }

        // Confidence gating (mirrors the LinkWatcher loop's proven thresholds).
        std::ostringstream clean;
        float confSum = 0;
        int confN = 0;
        for (const auto& l : page.lines) {
            if (l.confidence < 0.35f) continue;
            if (l.text.size() < 2 &&
                l.text.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") ==
                    std::string::npos) {
                continue;
            }
            clean << l.text << "\n";
            confSum += l.confidence;
            confN++;
        }
        std::string text = clean.str();
        if (!text.empty() && text.back() == '\n') text.pop_back();

        float meanConf = confN > 0 ? confSum / confN : 0.0f;
        bool meaningful = text.size() >= 4 && meanConf >= 0.35f;

        cycleRec.accepted = meaningful && !text.empty();
        cycleRec.rejected = !cycleRec.accepted;
        if (cycleRec.rejected) cycleRec.rejectionReason = "Below confidence gate";
        cycleRec.text = text;
        cycleRec.confidence = meanConf;
        cycleRec.latencyMs = latency;
        for (const auto& l : page.lines) {
            if (l.confidence < 0.35f) continue;
            OcrCycleRecord::Line line;
            line.text = l.text;
            line.confidence = l.confidence;
            line.x = static_cast<float>(l.x);
            line.y = static_cast<float>(l.y);
            line.width = static_cast<float>(l.width);
            line.height = static_cast<float>(l.height);
            cycleRec.lines.push_back(std::move(line));
        }

        std::string ocrPath = report_->saveOcrCycle(cycleRec, &pngBytes);
        pushLog("OCR_EXECUTION_COMPLETED: " + std::to_string(cycleRec.lines.size()) +
                " lines, conf=" + std::to_string(meanConf) + ", " +
                std::to_string(latency) + "ms, saved=" +
                (ocrPath.empty() ? std::string("(none)") : ocrPath));

        // The GUI "Pages saved" / "OCR accepted" counters move ONLY after a
        // real screenshot + real PP-OCRv3 result was accepted and the per-target
        // report file was actually written.
        if (!ocrPath.empty() && cycleRec.accepted && savedOcrCb_) {
            acceptedSaves_++;
            savedOcrCb_(cycleRec.cdpTargetId, cycleRec.url, cycleRec.title,
                        cycleRec.text, cycleRec.confidence);
        }

        std::this_thread::sleep_for(interval_);
    }
}

} // namespace edgemon
