#include "trading/ocr/TradingOcrSource.hpp"
#include "logger.hpp"

#include "edge/EdgeConnector.hpp"
#include "edge/PageSession.hpp"
#include "edge/TargetResolver.hpp"
#include "extraction/OcrEngine.hpp"
#include "capture/ChangeDetector.hpp"
#include "threadpool.hpp"

#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <thread>

namespace trading {

namespace {

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string decodeBase64(const std::string& b64) {
    // Base64 decode without external deps (subset used by CDP screenshot data).
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> rev(256, -1);
    for (size_t i = 0; i < tbl.size(); ++i) rev[static_cast<unsigned char>(tbl[i])] = static_cast<int>(i);
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : b64) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int d = rev[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

} // namespace

// Per-target worker state.
struct TradingOcrSource::Watcher {
    OcrWatchTarget cfg;
    std::thread thread;

    // Live session state.
    std::shared_ptr<edgemon::PageSession> session;
    std::string cdpTargetId;
    std::string webSocketUrl;
    std::string currentUrl;

    // Per-watcher OCR engine + change detector (one instance each).
    std::shared_ptr<edgemon::OcrEngine> ocr;
    std::unique_ptr<edgemon::FrameChangeDetector> changeDetector;

    uint64_t sequence = 0;
    OcrDeduplicator dedup;
    std::atomic<bool> stop{false};
};

TradingOcrSource::TradingOcrSource(TradingEventBus& bus) : bus_(bus) {}

TradingOcrSource::~TradingOcrSource() {
    stop();
}

void TradingOcrSource::configure(const std::vector<OcrWatchTarget>& targets) {
    targets_ = targets;
    watchers_.clear();
    for (const auto& t : targets_) {
        auto w = std::make_unique<Watcher>();
        w->cfg = t;
        w->ocr = std::make_shared<edgemon::OcrEngine>();
        watchers_.push_back(std::move(w));
    }
}

bool TradingOcrSource::start() {
    if (running_.exchange(true)) return false;
    if (watchers_.empty()) {
        running_.store(false);
        return false;
    }
    for (auto& w : watchers_) {
        w->stop.store(false);
        w->thread = std::thread([this, w = w.get()]() { watcherLoop(*w); });
    }
    return true;
}

void TradingOcrSource::stop() {
    if (!running_.exchange(false)) {
        // Still stop watcher threads if configure() was never started but
        // threads exist (edge case).
        for (auto& w : watchers_) {
            w->stop.store(true);
            if (w->thread.joinable()) w->thread.join();
        }
        return;
    }
    for (auto& w : watchers_) {
        w->stop.store(true);
        if (w->thread.joinable()) w->thread.join();
        if (w->session) w->session->disconnect();
    }
}

bool TradingOcrSource::attachSession(Watcher& w) {
    // 1. Ensure a CDP-attachable Edge instance (dedicated silent profile).
    auto probe = edgemon::EdgeConnector::ensureDedicatedCdpInstance(w.cfg.url, 20000);
    if (!probe.cdpAvailable()) {
        LOG_WARN("TradingOcrSource: no CDP for {}: {}", w.cfg.url,
                 edgemon::EdgeConnectorStateToString(probe.state));
        return false;
    }
    // 2. Discover the instance + resolve the target by exact URL.
    edgemon::EdgeDiscovery discovery;
    auto instances = discovery.discoverAllInstances();
    edgemon::EdgeTarget resolved;
    bool found = false;
    for (const auto& inst : instances) {
        auto t = discovery.findTargetByUrl(inst, edgemon::PageUrl(w.cfg.url));
        if (t) {
            resolved = *t;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_WARN("TradingOcrSource: target page not found for {}", w.cfg.url);
        return false;
    }
    w.cdpTargetId = resolved.id.value;
    w.webSocketUrl = resolved.webSocketDebuggerUrl;
    w.currentUrl = resolved.url.value;
    // 3. Open a PageSession.
    auto pool = std::make_shared<edgemon::ThreadPool>(2);
    auto session = std::make_shared<edgemon::PageSession>(
        edgemon::TargetId("trading-ocr"), edgemon::CDPTargetId(w.cdpTargetId),
        w.webSocketUrl, pool);
    if (!session->connect()) {
        LOG_WARN("TradingOcrSource: session connect failed for {}", w.cfg.url);
        return false;
    }
    w.session = session;
    return true;
}

void TradingOcrSource::emitEvent(Watcher& w, const OcrEvent& ev) {
    BusEvent be;
    be.type = EventType::OCR;
    be.source = "ocr:" + w.cfg.role;
    be.symbol = ev.symbols.empty() ? "" : ev.symbols[0];
    be.payload = nlohmann::json{{"page", ev.page},
                                {"text", ev.text},
                                {"confidence", ev.confidence},
                                {"type", toString(ev.type)},
                                {"role", w.cfg.role}}
                     .dump();
    bus_.publishAsync(be);
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.events++;
}

void TradingOcrSource::watcherLoop(Watcher& w) {
    // Load OCR models once.
    if (!w.ocr->loadModels("models")) {
        LOG_WARN("TradingOcrSource: OCR engine unavailable: {}",
                 w.ocr->lastError());
    }
    w.changeDetector = std::make_unique<edgemon::FrameChangeDetector>();

    auto intervalMs = static_cast<int>(1000.0 / w.cfg.rateHz);
    if (intervalMs < 200) intervalMs = 200;  // never hammer OCR faster than 5Hz

    bool attached = false;
    while (!w.stop.load()) {
        auto cycleStart = std::chrono::steady_clock::now();

        if (!attached || !w.session || !w.session->isConnected()) {
            if (w.session) { w.session->disconnect(); w.session.reset(); }
            attached = attachSession(w);
            if (!attached) {
                // Backoff with jitter; keep trying until stopped.
                int backoffMs = 2000 + static_cast<int>(rand() % 1000);
                for (int i = 0; i < backoffMs / 100 && !w.stop.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        // Capture screenshot.
        nlohmann::json shot;
        try {
            shot = w.session->captureScreenshot("png", 90, false);
        } catch (const std::exception& e) {
            LOG_WARN("TradingOcrSource: capture threw: {}", e.what());
            attached = false;
            continue;
        }
        if (shot.empty() || !shot.contains("data")) {
            LOG_DEBUG("TradingOcrSource: no screenshot data for {}", w.cfg.url);
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            continue;
        }
        std::string b64;
        try { b64 = shot["data"].get<std::string>(); } catch (...) { continue; }
        std::string png = decodeBase64(b64);

        // Decode to BGRA and run change detection.
        std::vector<uint8_t> bgra;
        int iw = 0, ih = 0;
        if (!edgemon::OcrEngine::decodePng(
                std::vector<uint8_t>(png.begin(), png.end()), bgra, iw, ih) ||
            iw == 0 || ih == 0) {
            LOG_DEBUG("TradingOcrSource: PNG decode failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            continue;
        }
        edgemon::CapturedFrame frame;
        frame.targetId = edgemon::TargetId("trading-ocr");
        frame.cdpTargetId = edgemon::CDPTargetId(w.cdpTargetId);
        frame.width = static_cast<uint32_t>(iw);
        frame.height = static_cast<uint32_t>(ih);
        frame.format = edgemon::PixelFormat::BGRA;
        frame.pixels = std::move(bgra);
        auto change = w.changeDetector->detect(frame);
        if (!change.hasChanged) {
            // No visual change — skip OCR (saves CPU + rate limits).
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            continue;
        }

        // Run OCR.
        if (!w.ocr->available()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            continue;
        }
        auto ocrStart = std::chrono::steady_clock::now();
        auto result = w.ocr->recognize(frame.pixels.data(), iw, ih);
        auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - ocrStart)
                             .count();

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.cycles++;
            stats_.lastLatencyMs = static_cast<double>(latencyMs);
        }
        if (result.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            continue;
        }

        // Emit per-line events with region boxes + confidence filter.
        OcrClassifier clf;
        for (const auto& line : result.lines) {
            if (line.text.empty()) continue;
            if (line.confidence < w.cfg.minConfidence) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.lowConfidence++;
                continue;
            }
            OcrEvent ev;
            ev.source = "edge:" + w.cfg.label;
            ev.page = w.cfg.url;
            ev.text = line.text;
            ev.timestampNs = nowNs();
            ev.confidence = line.confidence;
            ev.sequence = ++w.sequence;
            char region[64];
            std::snprintf(region, sizeof(region), "x=%d y=%d w=%d h=%d",
                          line.x, line.y, line.width, line.height);
            ev.region = region;

            ev.type = clf.classify(line.text, w.cfg.role);
            ev.symbols = clf.extractSymbols(line.text);
            ev.sentiment = clf.sentiment(line.text);
            ev.urgent = clf.isUrgent(line.text);
            ev.kind = w.cfg.role;

            // Dedup identical text from the same source within the window.
            std::string norm = line.text;
            norm.erase(std::remove_if(norm.begin(), norm.end(),
                                      [](unsigned char c) { return std::isspace(c); }),
                       norm.end());
            if (!w.dedup.isNew(ev.source, norm, ev.timestampNs)) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.deduped++;
                continue;
            }
            emitEvent(w, ev);
        }

        // Sleep the remainder of the interval.
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - cycleStart)
                           .count();
        int sleepMs = intervalMs - static_cast<int>(elapsed);
        if (sleepMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    if (w.session) w.session->disconnect();
}

OcrCaptureResult TradingOcrSource::ocrOnce(const OcrWatchTarget& target,
                                           int timeoutMs) {
    OcrCaptureResult res;
    edgemon::OcrEngine ocr;
    if (!ocr.loadModels("models")) {
        res.error = ocr.lastError();
        return res;
    }
    Watcher w;
    w.cfg = target;
    if (!attachSession(w)) {
        res.error = "no CDP/Edge target available";
        return res;
    }
    auto shot = w.session->captureScreenshot("png", 90, false);
    if (shot.empty() || !shot.contains("data")) {
        res.error = "capture returned no data";
        w.session->disconnect();
        return res;
    }
    std::string b64 = shot["data"].get<std::string>();
    std::string png = decodeBase64(b64);
    std::vector<uint8_t> bgra;
    int iw = 0, ih = 0;
    if (!edgemon::OcrEngine::decodePng(
            std::vector<uint8_t>(png.begin(), png.end()), bgra, iw, ih)) {
        res.error = "PNG decode failed";
        w.session->disconnect();
        return res;
    }
    auto result = ocr.recognize(bgra.data(), iw, ih);
    OcrClassifier clf;
    for (const auto& line : result.lines) {
        if (line.text.empty() || line.confidence < target.minConfidence) continue;
        OcrEvent ev;
        ev.source = "edge:" + target.label;
        ev.page = target.url;
        ev.text = line.text;
        ev.timestampNs = nowNs();
        ev.confidence = line.confidence;
        ev.sequence = ++w.sequence;
        ev.type = clf.classify(line.text, target.role);
        ev.symbols = clf.extractSymbols(line.text);
        ev.sentiment = clf.sentiment(line.text);
        ev.kind = target.role;
        res.events.push_back(std::move(ev));
    }
    res.ok = true;
    w.session->disconnect();
    return res;
}

TradingOcrSource::Stats TradingOcrSource::stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

} // namespace trading
