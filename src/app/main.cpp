// Edge Monitor â€” multi-target Edge browser monitoring with DOM/OCR extraction,
// embeddings, vector search, CLI subcommands, and a Win32 status GUI.
//
// Identity invariant: a monitored target is a unique internal TargetId
// registered in TargetRegistry, bound to a CDPTargetId + Edge process id.
// Never identified by HWND, active tab, screen coordinates, or URL alone.

#include "core/TargetManager.hpp"
#include "core/TargetRegistry.hpp"
#include "core/EventBus.hpp"
#include "edge/EdgeDiscovery.hpp"
#include "edge/TargetResolver.hpp"
#include "edge/PageSession.hpp"
#include "edge/EdgeConnector.hpp"
#include "edge/ExactTargetMonitor.hpp"
#include "edge/PerTargetReport.hpp"
#include "edge/UrlUtils.hpp"
#include "app/TargetSelection.hpp"
#include "capture/CaptureManager.hpp"
#include "extraction/DOMExtractor.hpp"
#include "extraction/OCRManager.hpp"
#include "extraction/OcrEngine.hpp"
#include "extraction/TextNormalizer.hpp"
#include "embedding/EmbeddingEngine.hpp"
#include "embedding/VectorStore.hpp"
#include "storage/SQLiteStore.hpp"
#include "common.hpp"
#include "logger.hpp"
#include "threadpool.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <chrono>
#include <atomic>
#include <csignal>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <optional>
#include <cstdlib>
#include <array>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <commctrl.h>
#include <shlwapi.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shlwapi.lib")
// NtQueryInformationProcess signature (ntdll).
typedef LONG NTSTATUS;
#endif

namespace edgemon {

// Forward decl: implemented below in this TU (uses the anon-namespace HTTP
// helpers). Opens a URL as a new tab in the CDP browser and returns its
// (cdpId, wsUrl) on success.
struct OpenedTab { std::string id; std::string webSocketUrl; };
OpenedTab openCdpTab(int cdpPort, const std::string& url);

// ============================================================================
// MonitorEngine â€” wires discovery, registry, sessions, capture, OCR, embeddings
// ============================================================================

class MonitorEngine {
public:
    MonitorEngine()
        : pool_(std::make_shared<ThreadPool>(4))
        , captureManager_(pool_)
        , embeddingEngine_(std::make_shared<EmbeddingEngine>())
        , vectorStore_(embeddingEngine_)
        , textNormalizer_(std::make_unique<TextNormalizer>())
        , running_(false)
        , initialized_(false)
        , stopped_(false) {}

    ~MonitorEngine() { stop(); }

    bool initialize() {
        Logger::initialize("EdgeMonitor", "logs/");
        LOG_INFO("=== Edge Monitor Starting ===");

        if (!sqliteStore_.initialize("edge_monitor.db")) {
            LOG_ERROR("Failed to initialize SQLite store");
            return false;
        }
        if (!embeddingEngine_->initialize()) {
            LOG_ERROR("Failed to initialize embedding engine");
            return false;
        }
        if (!vectorStore_.initialize()) {
            LOG_ERROR("Failed to initialize vector store");
            return false;
        }
        vectorStore_.setPersistencePath("vectors.json");

        targetManager_.initialize();
        loadPersistedTargets();

        ocrManager_ = std::make_shared<OCRManager>(pool_);
        ocrManager_->initialize();
        ocrManager_->setResultCallback([this](const OCRResult& result) {
            if (!result.rejected && !result.text.empty()) {
                ingestText(result.targetId, result.text, ContentSource::OCR, result.confidence);
            }
        });

        captureManager_.setFrameCallback([this](const CapturedFrame& frame) {
            onFrameCaptured(frame);
        });
        captureManager_.setOCRCallback([this](const CapturedFrame& frame) {
            ocrManager_->processFrame(frame, [](const OCRResult&) {});
        });

        targetManager_.setTargetCallback([this](const MonitoredTarget& target, TargetState state) {
            onTargetStateChanged(target, state);
        });

        EventBus::instance().subscribe(EventType::TextExtracted,
            [this](const Event& event) {
                const auto& content = std::get<std::string>(event.data);
                ingestText(TargetId(event.targetId), content, ContentSource::DOM, 1.0f);
            });

        initialized_ = true;
        LOG_INFO("Edge Monitor initialized successfully");
        return true;
    }

    void start() {
        if (!initialized_) return;
        if (running_.exchange(true)) return;
        captureManager_.start();
        pool_->submit([this]() { monitorLoop(); });
        LOG_INFO("Edge Monitor started");
    }

    void stop() {
        // Idempotent teardown: must work even if start() was never called
        // (CLI one-shot commands), and must be safe to call from the
        // destructor after an explicit stop().
        if (stopped_.exchange(true)) return;
        running_ = false;
        for (auto& [id, session] : sessions_) {
            if (session) session->disconnect();
        }
        sessions_.clear();
        captureManager_.stop();
        targetManager_.shutdown();
        if (ocrManager_) ocrManager_->shutdown();
        pool_->stop();
        LOG_INFO("Edge Monitor stopped");
    }

    // ---- Discovery / Locking -------------------------------------------------

    std::vector<EdgeTarget> discover() {
        auto result = discovery_.discover();
        if (!result.success) {
            LOG_WARN("Discovery failed: {}", result.errorMessage);
            return {};
        }
        resolver_.setInstances(result.instances);
        std::vector<EdgeTarget> pages;
        for (const auto& t : result.targets) {
            if (t.isPage() || t.isBackgroundPage()) pages.push_back(t);
        }
        LOG_INFO("Discovered {} page targets across {} Edge instances",
                 pages.size(), result.instances.size());
        return pages;
    }

    TargetId lockTarget(const std::string& url, bool enableOCR) {
        if (!initialized_) return TargetId();
        if (url.empty()) return TargetId();

        // Each CLI invocation is a fresh process: run discovery so the resolver
        // has the live Edge instances before resolving the URL.
        auto fresh = discovery_.discover();
        if (fresh.success) {
            resolver_.setInstances(fresh.instances);
        }

        auto result = resolver_.resolve(PageUrl(url));
        if (!result.found) {
            LOG_ERROR("Cannot resolve URL '{}': {}", url, result.errorMessage);
            return TargetId();
        }

        // Duplicate prevention at registry level (CDP id or URL already locked).
        // Even when already locked, make sure a live session is attached so
        // downstream code (e.g. LinkWatcher.evaluateOnTarget) can use it.
        if (registry_->existsByCDPId(result.target.id)) {
            LOG_WARN("Target already locked for CDP target {}", result.target.id.value);
            auto existing = registry_->getByCDPId(result.target.id);
            if (existing) attachSession(existing->id, *existing);
            return existing ? existing->id : TargetId();
        }
        if (auto byUrl = registry_->getByUrl(PageUrl(url)); byUrl) {
            LOG_WARN("Target already locked for URL {}", url);
            if (byUrl) attachSession(byUrl->id, *byUrl);
            return byUrl->id;
        }

        auto id = targetManager_.lockTarget(result.target);
        if (id.empty()) return id;

        auto target = registry_->get(id);
        if (target) {
            captureManager_.registerTarget(id.value);
            captureManager_.enableVisualCapture(id.value, enableOCR);
            if (enableOCR) {
                auto cfg = target->config;
                cfg.ocrEnabled = true;
                targetManager_.setMonitoringConfig(id, cfg);
            }
            attachSession(id, *target);
        }
        persistTarget(id);
        LOG_INFO("Locked target {} (CDP {})", id.value, result.target.id.value);
        return id;
    }

    bool unlockTarget(const std::string& id) {
        detachSession(TargetId(id));
        captureManager_.unregisterTarget(id);
        sqliteStore_.deleteTarget(id);
        return targetManager_.unlockTarget(TargetId(id));
    }

    // ---- Status --------------------------------------------------------------

    std::vector<MonitoredTarget> targets() const {
        return registry_->getAll();
    }

    TargetManager::TargetStats stats() const {
        return targetManager_.getStats();
    }

    size_t pendingFrames() const { return captureManager_.pendingFrames(); }
    size_t ocrQueue() const { return ocrManager_ ? ocrManager_->queueSize() : 0; }
    size_t vectorCount() const { return vectorStore_.count(); }
    size_t sqliteCount() const { return sqliteStore_.count(); }

    OCRManager::Stats ocrStats() const {
        return ocrManager_ ? ocrManager_->getStats() : OCRManager::Stats{};
    }

    EmbeddingEngine::Stats embeddingStats() const {
        return embeddingEngine_->getStats();
    }

    std::shared_ptr<ThreadPool> pool() const { return pool_; }

    // Runs JS on a locked target's live session (waits for the session to
    // connect, up to ~40s). Returns the serialized result value, or "".
    std::string evaluateOnTarget(const TargetId& targetId, const std::string& script) {
        std::shared_ptr<PageSession> session;
        for (int attempt = 0; attempt < 80; ++attempt) {
            {
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                auto it = sessions_.find(targetId);
                if (it != sessions_.end() && it->second && it->second->isConnected()) {
                    session = it->second;
                }
            }
            if (session) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!session) {
            LOG_WARN("evaluateOnTarget: no live session for {}", targetId.value);
            return "";
        }
        auto res = session->executeJavaScriptSync(script, std::chrono::seconds(10));
        if (!res) return "";
        try {
            if (res->contains("result") && (*res)["result"].contains("value")) {
                auto& v = (*res)["result"]["value"];
                if (v.is_string()) return v.get<std::string>();
                return v.dump();
            }
        } catch (const std::exception&) {
        }
        return "";
    }

    // ---- OCR / Search --------------------------------------------------------

    void runOCR(const std::string& targetId) {
        auto target = registry_->get(TargetId(targetId));
        if (!target) {
            std::cout << "Target not found: " << targetId << std::endl;
            return;
        }
        auto it = sessions_.find(target->id);
        if (it == sessions_.end() || !it->second || !it->second->isConnected()) {
            std::cout << "Target " << targetId << " has no live session" << std::endl;
            return;
        }
        std::cout << "Requesting screenshot + OCR for " << targetId << std::endl;
        try {
            nlohmann::json shot = it->second->captureScreenshot("png", 90, false);
            if (shot.contains("data")) {
                CapturedFrame frame;
                frame.targetId = target->id;
                frame.cdpTargetId = target->cdpTargetId;
                frame.timestamp = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                frame.status = CaptureStatus::Success;
                ocrManager_->processFrame(frame, [](const OCRResult& r) {
                    if (r.rejected) {
                        std::cout << "  OCR rejected: " << r.rejectionReason << std::endl;
                    } else {
                        std::cout << "  OCR text (" << r.confidence << "): "
                                  << r.text.substr(0, 120) << std::endl;
                    }
                });
            }
        } catch (const std::exception& e) {
            std::cout << "OCR capture failed: " << e.what() << std::endl;
        }
    }

    std::vector<SearchResult> search(const std::string& query, size_t topK) {
        return vectorStore_.search(query, topK);
    }

    // ---- Exact-target watch (primary) --------------------------------------

    // Resolves the exact page (or falls back to user tab selection), persists
    // the target + URL rule, and starts a continuous change-gated OCR monitor
    // writing to reports/<target-id>/. Returns the started monitor (caller
    // owns it), or nullptr when no authenticated/CDP-attachable target exists.
    std::shared_ptr<ExactTargetMonitor> startExactWatch(
        const std::string& requestedUrl, bool interactive,
        const std::function<void(const std::string&)>& logCb);

    // Re-attach persisted targets after an Edge restart (monitorLoop helper).
    void refreshPersistedSessions();

    void ingestText(const TargetId& targetId, const std::string& text,
                    ContentSource source, float confidence) {
        if (text.empty()) return;
        auto normalized = textNormalizer_->normalize(text);
        if (normalized.empty()) return;
        auto hash = textNormalizer_->computeHash(normalized);

        VectorRecord record;
        record.id = "vec_" + std::to_string(Clock::now().time_since_epoch().count());
        record.targetId = targetId.value;
        record.content = normalized;
        record.timestamp = Clock::now();
        record.timestampEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            record.timestamp.time_since_epoch()).count();
        record.embedding = embeddingEngine_->embed(normalized);
        record.url = "";
        vectorStore_.add(record);

        // Dedup against SQLite AFTER the vector add: a record persisted in a
        // previous run must not block the (possibly missing) vector write.
        if (sqliteStore_.recordExists(hash)) return;

        StoredRecord dbRecord;
        dbRecord.id = record.id;
        dbRecord.targetId = record.targetId;
        dbRecord.content = record.content;
        dbRecord.timestamp = record.timestampEpoch;
        dbRecord.contentHash = hash;
        sqliteStore_.saveRecord(dbRecord);

        // Stable uint64 hash for registry change tracking (the string hash is
        // kept for SQLite content-hash dedup).
        uint64_t changeHash = 0;
        for (char c : hash) {
            changeHash = changeHash * 131 + static_cast<unsigned char>(c);
        }
        registry_->updateLastHash(targetId, changeHash, 0);
        LOG_INFO("[{}] ingested {} chars (source={}, conf={:.2f})",
                 targetId.value, normalized.size(),
                 source == ContentSource::OCR ? "OCR" : "DOM", confidence);
    }

    // ---- Internals -----------------------------------------------------------

private:
    void persistTarget(const TargetId& id) {
        auto target = registry_->get(id);
        if (!target) return;
        SQLiteStore::TargetRow row;
        row.id = target->id.value;
        row.cdpTargetId = target->cdpTargetId.value;
        row.processId = target->edgeProcessId.value;
        row.title = target->title;
        row.url = target->url.value;
        row.webSocketUrl = target->webSocketUrl;
        row.ocrEnabled = target->config.ocrEnabled;
        row.state = TargetStateToString(target->state);
        row.stateMessage = target->stateMessage;
        row.targetRule = TargetMatchRuleToString(target->targetRule);
        row.profileHint = target->profileHint;
        sqliteStore_.saveTarget(row);
    }

    void loadPersistedTargets() {
        auto rows = sqliteStore_.loadTargets();
        for (const auto& row : rows) {
            if (registry_->exists(TargetId(row.id))) continue;

            MonitoredTarget target;
            target.id = TargetId(row.id);
            target.cdpTargetId = CDPTargetId(row.cdpTargetId);
            target.edgeProcessId = EdgeProcessId(row.processId);
            target.title = row.title;
            target.url = PageUrl(row.url);
            target.webSocketUrl = row.webSocketUrl;
            target.enabled = true;
            target.config.domEnabled = true;
            target.config.visualEnabled = true;
            target.config.ocrEnabled = row.ocrEnabled;
            target.config.mode = CaptureMode::Hybrid;
            target.state = TargetState::Discovered;
            if (auto rule = TargetSelection::ruleFromString(row.targetRule)) {
                target.targetRule = *rule;
            }
            target.profileHint = row.profileHint;
            registry_->add(target);
            LOG_INFO("Restored persisted target {} (CDP {} rule={})",
                     row.id, row.cdpTargetId,
                     row.targetRule.empty() ? "exact" : row.targetRule);
        }
    }


    void attachSession(const TargetId& id, const MonitoredTarget& target) {
        if (target.cdpTargetId.empty() || target.webSocketUrl.empty()) return;
        auto session = std::make_shared<PageSession>(
            id, target.cdpTargetId, target.webSocketUrl, pool_);
        session->setDOMCallback([this, id](const std::string& text) {
            ingestText(id, text, ContentSource::DOM, 1.0f);
        });
        session->setNavigationCallback([this, id](const std::string& url, const std::string& title) {
            LOG_INFO("[{}] navigated -> {} ({})", id.value, url, title);
            registry_->setState(id, TargetState::Connected);
        });
        session->setErrorCallback([this, id](const std::string& error, bool recoverable) {
            LOG_WARN("[{}] session error (recoverable={}): {}",
                     id.value, recoverable, error);
            if (!recoverable) {
                registry_->setState(id, TargetState::Offline, error);
            }
        });
        if (session->connect()) {
            session->enablePageEvents();
            session->enableDOMMonitoring();
            {
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                sessions_[id] = session;
            }
            registry_->setState(id, TargetState::Monitoring);

            // Initial DOM extraction: ingest the page's current text once so
            // fresh content is indexed even before the first mutation event.
            pool_->submit([this, id, session]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                try {
                    auto text = session->getDocumentContent();
                    if (!text.empty()) {
                        ingestText(id, text, ContentSource::DOM, 1.0f);
                        LOG_INFO("[{}] initial DOM ingested ({} chars)",
                                 id.value, text.size());
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("[{}] initial DOM extraction failed: {}",
                             id.value, e.what());
                }
            });
        } else {
            LOG_WARN("[{}] session connect failed", id.value);
            registry_->setState(id, TargetState::Reconnecting, "connect failed");
        }
    }

    void detachSession(const TargetId& id) {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            if (it->second) it->second->disconnect();
            sessions_.erase(it);
        }
    }

    void monitorLoop() {
        while (running_) {
            // Re-attach sessions for targets that have a websocket URL but no
            // live session (e.g. after reconnect/re-resolution).
            auto all = registry_->getAll();
            for (const auto& target : all) {
                if (!target.enabled) continue;
                {
                    std::lock_guard<std::mutex> lock(sessionsMutex_);
                    auto it = sessions_.find(target.id);
                    if (it != sessions_.end() && it->second && it->second->isConnected()) {
                        continue;
                    }
                }
                if (target.state == TargetState::Monitoring) {
                    registry_->setState(target.id, TargetState::Reconnecting, "session lost");
                }
                auto fresh = discovery_.discover();
                if (fresh.success) {
                    resolver_.setInstances(fresh.instances);
                    auto resolved = resolver_.resolveByExactMatch(target.url);
                    if (resolved.found) {
                        MonitoredTarget updated = target;
                        updated.cdpTargetId = resolved.target.id;
                        updated.webSocketUrl = resolved.target.webSocketDebuggerUrl;
                        updated.edgeProcessId = resolved.target.processId;
                        registry_->update(updated);
                        attachSession(target.id, updated);
                    } else {
                        registry_->setState(target.id, TargetState::Offline, "target not found");
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    void onFrameCaptured(const CapturedFrame& frame) {
        LOG_DEBUG("Frame captured for target {}", frame.targetId.value);
    }

    void onTargetStateChanged(const MonitoredTarget& target, TargetState state) {
        LOG_INFO("[{}] state -> {}", target.id.value, TargetStateToString(state));
    }

    std::shared_ptr<ThreadPool> pool_;
    TargetManager targetManager_;
    TargetRegistry* registry_ = &TargetRegistry::instance();
    CaptureManager captureManager_;
    EdgeDiscovery discovery_;
    TargetResolver resolver_;

    std::shared_ptr<EmbeddingEngine> embeddingEngine_;
    VectorStore vectorStore_;
    std::unique_ptr<TextNormalizer> textNormalizer_;
    std::shared_ptr<OCRManager> ocrManager_;
    SQLiteStore sqliteStore_;

    mutable std::mutex sessionsMutex_;
    std::map<TargetId, std::shared_ptr<PageSession>> sessions_;

    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    std::atomic<bool> stopped_;
};

// ============================================================================
// MonitorEngine out-of-line: exact-target watch + persisted-session recovery
// ============================================================================

std::shared_ptr<ExactTargetMonitor> MonitorEngine::startExactWatch(
    const std::string& requestedUrl, bool interactive,
    const std::function<void(const std::string&)>& logCb) {
    auto narrow = [](const std::wstring& ws) {
        std::string out;
        out.reserve(ws.size());
        for (wchar_t c : ws) out.push_back(static_cast<char>(c & 0xFF));
        return out;
    };
    // 1) Ensure a genuinely CDP-attachable browser. Prefer a live CDP endpoint
    //    if one exists; otherwise auto-launch a dedicated Edge instance on a
    //    NON-DEFAULT profile (the only way Chromium >= 136 allows the port),
    //    which coexists with the user's normal browsing.
    EdgeProbeResult probe = EdgeConnector::ensureDedicatedCdpInstance();
    if (!probe.cdpAvailable()) {
        EdgeProbeResult diag = EdgeConnector::diagnose();
        std::string reason;
        for (const auto& r : diag.reasons) reason += " " + r;
        if (logCb) {
            logCb("AUTHENTICATED_SESSION_UNAVAILABLE: could not enable a CDP "
                  "browser." + reason);
        }
        LOG_ERROR("startExactWatch: {}", reason);
        return nullptr;
    }
    if (logCb) {
        logCb("CDP_AVAILABLE port=" + std::to_string(probe.cdpPort) +
              " browser=" + probe.browserVersion);
    }

    // 2) Discover + resolve the exact target deterministically.
    auto discovery = std::make_shared<EdgeDiscovery>();
    EdgeDiscovery::DiscoveryConfig cfg;
    cfg.preferredPorts = {probe.cdpPort};
    discovery->setConfig(cfg);
    auto instances = discovery->discoverAllInstances();
    TargetResolver resolver;
    resolver.setInstances(instances);

    TargetMatchRule rule = TargetMatchRule::Exact;
    auto resolved = resolver.resolve(PageUrl(requestedUrl), rule);

    EdgeTarget chosen;
    std::string ruleUrl = requestedUrl;
    if (!resolved.found) {
        // The requested URL is not open in the CDP instance yet. Open it now
        // (PUT /json/new?<url>) so "watch <url>" works against the dedicated
        // Edge instead of failing with TARGET_NOT_FOUND.
        OpenedTab openedTab;
        if (!requestedUrl.empty()) {
            openedTab = openCdpTab(probe.cdpPort, requestedUrl);
            if (!openedTab.id.empty() && logCb) {
                logCb("TARGET_NOT_FOUND: opened '" + requestedUrl +
                      "' in CDP browser (new tab).");
            }
        }
        if (!openedTab.id.empty()) {
            // Re-resolve against the now-open target (a few tries for load).
            for (int attempt = 0; attempt < 6; ++attempt) {
                auto freshInstances = discovery->discoverAllInstances();
                resolver.setInstances(freshInstances);
                auto re = resolver.resolve(PageUrl(requestedUrl), rule);
                if (re.found) {
                    chosen = re.target;
                    ruleUrl = chosen.url.value;
                    rule = re.rule;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
            if (chosen.id.empty()) {
                if (logCb) {
                    logCb("TARGET_NOT_FOUND: opened tab but could not resolve '" +
                          requestedUrl + "'.");
                }
                return nullptr;
            }
        } else if (!interactive) {
            if (logCb) {
                logCb("TARGET_NOT_FOUND: '" + requestedUrl +
                      "' is not open. Use --select to pick from open tabs.");
            }
            return nullptr;
        } else {
            auto pages = TargetResolver::filterPageTargets(resolver.getAllTargets());
            auto sel = TargetSelection::promptCli(pages);
            if (!sel) {
                if (logCb) logCb("TARGET_NOT_FOUND: no tab selected.");
                return nullptr;
            }
            chosen = sel->target;
            ruleUrl = sel->ruleUrl.value;
            rule = sel->rule;
            if (logCb) {
                logCb("TargetResolver: selected_cdp_target=" + chosen.id.value +
                      " selected_url=" + chosen.url.value);
            }
        }
    } else {
        chosen = resolved.target;
        ruleUrl = chosen.url.value;
        rule = resolved.rule;
    }

    // 3) Persist as a locked target (durable URL rule, never only a CDP id).
    TargetId lockedId = targetManager_.lockTarget(chosen);
    if (lockedId.empty()) {
        if (logCb) logCb("TARGET_FOUND but lock failed for " + ruleUrl);
        return nullptr;
    }
    if (auto t = registry_->get(lockedId); t) {
        MonitoredTarget updated = *t;
        updated.url = PageUrl(ruleUrl);
        updated.targetRule = rule;
        updated.title = chosen.title;
        auto profile = EdgeConnector::findEdgeProfile();
        if (!profile.userDataDir.empty()) {
            updated.profileHint = narrow(profile.userDataDir);
        }
        registry_->update(updated);
    }
    persistTarget(lockedId);
    if (logCb) {
        logCb("TARGET_FOUND: locked " + lockedId.value + " rule=" +
              TargetMatchRuleToString(rule) + " url=" + ruleUrl);
    }

    // 4) Start the exact-target monitor + report.
    auto report = std::make_shared<PerTargetReport>();
    auto profile = EdgeConnector::findEdgeProfile();
    report->open(lockedId, chosen.id.value, ruleUrl, chosen.title, rule,
                 profile.userDataDir.empty() ? std::string()
                                             : narrow(profile.userDataDir));

    ExactTargetMonitor::TargetInfo info;
    info.internalId = lockedId;
    info.cdpTargetId = chosen.id;
    info.webSocketUrl = chosen.webSocketDebuggerUrl;
    info.url = PageUrl(ruleUrl);
    info.title = chosen.title;
    info.rule = rule;
    auto monitor = std::make_shared<ExactTargetMonitor>(pool_, info, report);
    if (logCb) monitor->setLogCallback(logCb);
    monitor->start();
    if (logCb) {
        logCb("watch active for " + ruleUrl + " — reports in " +
              report->rootPath());
    }
    return monitor;
}

void MonitorEngine::refreshPersistedSessions() {
    auto all = registry_->getAll();
    for (const auto& target : all) {
        if (!target.enabled) continue;
        if (target.cdpTargetId.empty() || target.webSocketUrl.empty()) continue;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            auto it = sessions_.find(target.id);
            if (it != sessions_.end() && it->second && it->second->isConnected()) {
                continue;
            }
        }
        auto fresh = discovery_.discover();
        if (!fresh.success) continue;
        resolver_.setInstances(fresh.instances);
        auto resolved = resolver_.resolve(target.url, target.targetRule);
        if (!resolved.found) {
            registry_->setState(target.id, TargetState::Offline,
                                "TARGET_NOT_FOUND after restart");
            continue;
        }
        MonitoredTarget updated = target;
        updated.cdpTargetId = resolved.target.id;
        updated.webSocketUrl = resolved.target.webSocketDebuggerUrl;
        updated.edgeProcessId = resolved.target.processId;
        registry_->update(updated);
        attachSession(updated.id, updated);
        LOG_INFO("refreshPersistedSessions: reattached {} via rule {} -> CDP {}",
                 target.id.value, TargetMatchRuleToString(target.targetRule),
                 resolved.target.id.value);
    }
}

// ============================================================================
// CLI helpers
// ============================================================================

static void printTargets(const std::vector<MonitoredTarget>& targets) {
    std::cout << "\n=== Targets ===" << std::endl;
    std::cout << "Total: " << targets.size() << std::endl;
    for (const auto& t : targets) {
        std::cout << "  [" << t.id.value << "] "
                  << TargetStateToString(t.state) << " | "
                  << t.title << " | " << t.url.value
                  << " | CDP:" << t.cdpTargetId.value
                  << " | PID:" << t.edgeProcessId.value
                  << std::endl;
    }
    std::cout << std::endl;
}

static void printStatus(MonitorEngine& engine) {
    auto stats = engine.stats();
    std::cout << "\n=== Edge Monitor Status ===" << std::endl;
    std::cout << "Targets: " << stats.totalLocked
              << " (online " << stats.online
              << ", offline " << stats.offline
              << ", monitoring " << stats.monitoring
              << ", error " << stats.error << ")" << std::endl;
    std::cout << "Frame queue: " << engine.pendingFrames() << std::endl;
    std::cout << "OCR queue: " << engine.ocrQueue() << std::endl;
    auto os = engine.ocrStats();
    std::cout << "OCR processed: " << os.totalProcessed
              << ", accepted: " << os.totalAccepted
              << ", rejected: " << os.totalRejected << std::endl;
    auto es = engine.embeddingStats();
    std::cout << "Embeddings: " << es.totalEmbeddings << std::endl;
    std::cout << "Vector records: " << engine.vectorCount() << std::endl;
    std::cout << "SQLite records: " << engine.sqliteCount() << std::endl;
    std::cout << std::endl;
}

static void printDiscovery(const std::vector<EdgeTarget>& targets) {
    std::cout << "\n=== Edge Pages ===" << std::endl;
    if (targets.empty()) {
        std::cout << "No Edge pages found. Is Edge running with --remote-debugging-port?" << std::endl;
        return;
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& t = targets[i];
        std::cout << "  [" << (i + 1) << "] " << t.title
                  << " | " << t.url.value
                  << " | CDP:" << t.id.value
                  << " | PID:" << t.processId.value
                  << std::endl;
    }
    std::cout << std::endl;
}

static void printHelp() {
    std::cout <<
        "Edge Monitor CLI\n"
        "Usage: edge-monitor.exe <command> [args]\n\n"
        "Commands:\n"
        "  discover              List Edge pages currently open\n"
        "  targets               List locked (monitored) targets\n"
        "  lock <url> [ocr]      Lock a target by URL (optionally enable OCR)\n"
        "  unlock <target-id>    Unlock a target\n"
        "  start                 Start the monitoring engine\n"
        "  stop                  Stop the monitoring engine\n"
        "  status                Show system status\n"
        "  ocr <target-id>       Trigger a screenshot + OCR pass on a target\n"
        "  search <query>        Semantic search over observed memory\n"
        "  watch <url> [--select] Monitor the EXACT page (already open, CDP\n"
        "                        enabled). --select lists open tabs to choose from.\n"
        "                        OCR -> reports/<target-id>/. Ctrl+C to stop.\n"
        "  watch-links <url>     (secondary) watch a page and all its links;\n"
        "                        saves to save-result/. Requires live CDP.\n"
        "  gui                   Open the Windows status GUI\n"
        "  help                  Show this help\n"
        "  --force               Run even if another instance is active\n";
}

// ============================================================================
// Result Saver â€” writes OCR results to save-result/ with 2-day auto-cleanup
// ============================================================================

namespace {

// Minimal base64 decode (same algorithm as BrowserCapture::decodeBase64Image,
// kept local so the watcher does not depend on BrowserCapture internals).
std::vector<uint8_t> decodeBase64Image(const std::string& base64Data) {
    std::vector<uint8_t> decoded;
    if (base64Data.empty()) return decoded;
    static const char* base64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    size_t padding = 0;
    if (base64Data.size() >= 1 && base64Data.back() == '=') padding++;
    if (base64Data.size() >= 2 && base64Data[base64Data.size() - 2] == '=') padding++;
    decoded.reserve(base64Data.size() * 3 / 4);
    std::array<int, 256> lookup{};
    lookup.fill(-1);
    for (int i = 0; i < 64; i++) {
        lookup[static_cast<unsigned char>(base64Chars[i])] = i;
    }
    std::array<uint8_t, 4> buffer{};
    size_t bufferPos = 0;
    for (size_t i = 0; i < base64Data.size(); i++) {
        char c = base64Data[i];
        if (c == '=') break;
        int val = lookup[static_cast<unsigned char>(c)];
        if (val < 0) continue;
        buffer[bufferPos++] = static_cast<uint8_t>(val);
        if (bufferPos == 4) {
            decoded.push_back((buffer[0] << 2) | (buffer[1] >> 4));
            decoded.push_back((buffer[1] << 4) | (buffer[2] >> 2));
            decoded.push_back((buffer[2] << 6) | buffer[3]);
            bufferPos = 0;
        }
    }
    if (bufferPos > 0) {
        for (size_t i = bufferPos; i < 4; i++) buffer[i] = 0;
        decoded.push_back((buffer[0] << 2) | (buffer[1] >> 4));
        if (bufferPos > 1) decoded.push_back((buffer[1] << 4) | (buffer[2] >> 2));
    }
    decoded.resize(decoded.size() - padding);
    return decoded;
}

// Minimal HTTP request over a raw socket (mirrors EdgeDiscovery's socketHttpGet;
// local so the watcher can call Edge's /json/new without exposing that internal
// helper). Returns true if an HTTP body was read.
bool httpRequest(const std::string& host, int port, const std::string& method,
                 const std::string& path, int timeoutMs, std::string& outBody) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    bool connected = false;
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        connected = true;
    } else {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(sock, &wset);
            if (select(0, nullptr, &wset, nullptr, &tv) > 0 && FD_ISSET(sock, &wset)) {
                int soerr = 0;
                int len = sizeof(soerr);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
                connected = (soerr == 0);
            }
        }
    }
    if (!connected) {
        closesocket(sock);
        return false;
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                      std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    int sent = 0;
    while (sent < static_cast<int>(req.size())) {
        int n = send(sock, req.c_str() + sent, static_cast<int>(req.size()) - sent, 0);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                timeval tv{};
                tv.tv_sec = timeoutMs / 1000;
                tv.tv_usec = (timeoutMs % 1000) * 1000;
                fd_set wset;
                FD_ZERO(&wset);
                FD_SET(sock, &wset);
                if (select(0, nullptr, &wset, nullptr, &tv) <= 0) {
                    closesocket(sock);
                    return false;
                }
                continue;
            }
            closesocket(sock);
            return false;
        }
        sent += n;
    }
    outBody.clear();
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs * 4);
    while (true) {
        if (std::chrono::steady_clock::now() > deadline) break;
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            outBody.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
                timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = 100000;
                fd_set rset;
                FD_ZERO(&rset);
                FD_SET(sock, &rset);
                select(0, &rset, nullptr, nullptr, &tv);
                continue;
            }
            break;
        }
    }
    closesocket(sock);
    return !outBody.empty();
}

// Extract the JSON body after the HTTP header.
std::string httpBodyOnly(const std::string& raw) {
    auto pos = raw.find("\r\n\r\n");
    if (pos == std::string::npos) pos = raw.find("\n\n");
    if (pos == std::string::npos) return raw;
    return raw.substr(pos + 4);
}

// Normalizes a user-pasted URL: trims, adds scheme if missing, strips a
// trailing slash (except root), so it can be matched against tab URLs.
std::string normalizeUrl(std::string url) {
    // Trim whitespace.
    auto trim = [](std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };
    url = trim(std::move(url));
    if (url.empty()) return url;

    // Add https:// when no scheme present.
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        // If it looks like "host/path" (contains a dot or localhost) prepend.
        if (url.find('.') != std::string::npos || url.rfind("localhost", 0) == 0) {
            url = "https://" + url;
        }
    }

    // Strip a single trailing slash (keep root "/").
    if (url.size() > 1 && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

std::string safeDirName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "page";
    if (out.size() > 60) out = out.substr(0, 60);
    return out;
}

// Splits a user-pasted list of URLs (whitespace / newline / comma separated)
// into individually-normalized non-empty URLs.
std::vector<std::string> splitUrls(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
            if (!cur.empty()) {
                std::string u = normalizeUrl(cur);
                if (!u.empty()) out.push_back(u);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        std::string u = normalizeUrl(cur);
        if (!u.empty()) out.push_back(u);
    }
    return out;
}

std::string timestampStr(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()) {
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

// Writes "<root>/save-result/<page>/<timestamp>.txt" for each OCR result, and
// periodically removes files older than 2 days so disk usage stays bounded.
class ResultSaver {
public:
    explicit ResultSaver(std::string root = "save-result")
        : root_(std::move(root)) {
        cleanupOld(48);  // startup sweep
        sweepThread_ = std::jthread([this](std::stop_token token) {
            while (!token.stop_requested() && !stopped_.load()) {
                std::unique_lock<std::mutex> lock(sweepMutex_);
                sweepCv_.wait_for(lock, std::chrono::hours(1));
                if (token.stop_requested() || stopped_.load()) break;
                cleanupOld(48);
            }
        });
    }

    ~ResultSaver() {
        {
            std::lock_guard<std::mutex> lock(sweepMutex_);
            stopped_ = true;
        }
        sweepCv_.notify_all();
        if (sweepThread_.joinable()) {
            sweepThread_.request_stop();
            sweepThread_.join();
        }
    }

    // Returns the full path of the written file, or "" on failure.
    std::string save(const TargetId& targetId, const std::string& pageName,
                     const std::string& text, float confidence) {
        if (text.empty()) return "";

        std::string dir = root_ + "/" + safeDirName(pageName);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("ResultSaver: cannot create {}: {}", dir, ec.message());
            return "";
        }

        std::string path = dir + "/" + timestampStr() + ".txt";
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            LOG_ERROR("ResultSaver: cannot open {}", path);
            return "";
        }
        out << "=== Edge Monitor OCR Result ===\n"
            << "Target:  " << targetId.value << "\n"
            << "Time:    " << timestampStr() << "\n"
            << "Page:    " << pageName << "\n"
            << "Conf:    " << std::fixed << std::setprecision(2) << confidence << "\n"
            << "----------------------------------------\n"
            << text << "\n";
        out.close();
        LOG_INFO("Saved OCR result: {} ({} chars)", path, text.size());
        return path;
    }

    // Removes files under root_ older than maxAgeHours (recursive).
    void cleanupOld(int maxAgeHours) {
        auto cutoff = std::chrono::system_clock::now() -
                      std::chrono::hours(maxAgeHours);
        std::error_code ec;
        if (!std::filesystem::exists(root_, ec)) return;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root_, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            auto ftime = std::filesystem::last_write_time(entry.path(), ec);
            if (ec) continue;
            auto ftp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            if (ftp < cutoff) {
                std::filesystem::remove(entry.path(), ec);
                LOG_INFO("ResultSaver: deleted old result {}", entry.path().string());
            }
        }
    }

private:
    std::string root_;
    std::atomic<bool> stopped_{false};
    std::mutex sweepMutex_;
    std::condition_variable sweepCv_;
    std::jthread sweepThread_;
};

// ============================================================================
// LinkWatcher — takes a locked page, enumerates all links, opens each in a tab,
// and runs a fast, dedicated OCR loop per page writing into save-result/.
// ============================================================================

// Forward decls of the live-state helpers (defined below LinkWatcher; used by
// its OCR loop to feed the GUI terminal + report in real time).
namespace live {
inline void pushLog(const std::string& line);
inline void updatePage(const std::string& key, const std::string& url,
                       const std::string& text, const std::string& file,
                       float confidence);
inline void setWatching(bool on);
} // namespace live

class LinkWatcher {
public:
    LinkWatcher(MonitorEngine& engine, std::string baseUrl, std::shared_ptr<ThreadPool> pool)
        : engine_(engine)
        , baseUrl_(std::move(baseUrl))
        , pool_(std::move(pool))
        , saver_()
        , running_(false) {
    }

    ~LinkWatcher() { stop(); }

    void start() {
        if (running_.exchange(true)) return;

        std::cout << "LinkWatcher: resolving base page " << baseUrl_ << " ..." << std::endl;

        // 0) Ensure a genuinely CDP-attachable browser. Prefer a live CDP
        //    endpoint if one exists; otherwise auto-launch a dedicated Edge
        //    instance on a NON-DEFAULT profile (the only way Chromium >= 136
        //    allows the port), coexisting with the user's normal browsing.
        EdgeProbeResult probe = EdgeConnector::ensureDedicatedCdpInstance(baseUrl_);
        if (!probe.cdpAvailable()) {
            EdgeProbeResult diag = EdgeConnector::diagnose();
            std::string reason;
            for (const auto& r : diag.reasons) reason += " " + r;
            std::string msg = std::string(EdgeConnectorStateToString(diag.state)) +
                              ": could not enable a CDP browser." + reason;
            std::cerr << "LinkWatcher: " << msg << std::endl;
            LOG_ERROR("LinkWatcher: {}", msg);
            live::pushLog(msg);
            running_ = false;
            return;
        }

        // 1) Find the base page's CDP target directly (no engine session).
        //    Retry with bounded backoff: the freshly launched Edge may still be
        //    loading the page, and discovery may briefly return no targets.
        std::optional<EdgeTarget> baseTarget;
        for (int attempt = 0; attempt < 5 && !baseTarget; ++attempt) {
            auto pages = engine_.discover();
            for (const auto& t : pages) {
                if (t.type == "page" &&
                    UrlUtils::matches(baseUrl_, t.url.value,
                                      TargetMatchRule::Exact)) {
                    baseTarget = t;
                    break;
                }
            }
            if (!baseTarget && attempt < 4) {
                LOG_WARN("LinkWatcher: base page not found yet (attempt {}/5); retrying...", attempt + 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            }
        }
        if (!baseTarget) {
            std::string msg = "TARGET_NOT_FOUND: cannot find '" + baseUrl_ +
                              "' among open Edge tabs. "
                              "Open the exact page in the CDP-enabled Edge and click Watch again.";
            std::cerr << "LinkWatcher: " << msg << std::endl;
            LOG_ERROR("LinkWatcher: {}", msg);
            live::pushLog(msg);
            running_ = false;
            return;
        }
        std::cout << "LinkWatcher: base page CDP target " << baseTarget->id.value << std::endl;

        // 2) Connect a dedicated session to the base page and read its links.
        std::cout << "LinkWatcher: collecting links..." << std::endl;
        std::vector<std::string> links = collectLinks(*baseTarget);
        std::cout << "LinkWatcher: found " << links.size() << " links" << std::endl;
        if (links.empty()) {
            LOG_WARN("LinkWatcher: no links found on {}", baseUrl_);
        }

        // 3) Start the OCR loop NOW on whatever tabs we can attach immediately
        //    (links already open), then open new tabs in the background so OCR
        //    is never blocked on slow tab creation. The base page is also
        //    watched via its dedicated session.
        attachWatchedTab(baseTarget->id, baseTarget->webSocketDebuggerUrl, baseUrl_);
        auto alreadyOpen = alreadyOpenTabs(links);
        for (const auto& [url, cdpId, wsUrl] : alreadyOpen) {
            attachWatchedTab(cdpId, wsUrl, url);
        }
        std::cout << "LinkWatcher: OCR loop starting on "
                  << (1 + alreadyOpen.size()) << " tabs immediately" << std::endl;

        // Load the real OCR engine once (PP-OCRv3 via ONNX Runtime). If the
        // models/DLL are missing, the loop logs "engine unavailable" and skips
        // frames rather than fabricating text.
        ocrEngine_ = std::make_shared<OcrEngine>();
        if (ocrEngine_->loadModels("models")) {
            LOG_INFO("LinkWatcher: real OCR engine loaded (PP-OCRv3)");
        } else {
            LOG_ERROR("LinkWatcher: real OCR engine unavailable: {}",
                      ocrEngine_->lastError());
        }

        ocrThread_ = std::jthread([this](std::stop_token token) { ocrLoop(token); });

        std::cout << "LinkWatcher: opening remaining tabs in background..." << std::endl;
        // Open new tabs (bounded, fast) and attach as they come up.
        std::thread([this, links]() {
            auto opened = openAllTabs(links);
            for (const auto& [url, cdpId, wsUrl] : opened) {
                attachWatchedTab(cdpId, wsUrl, url);
            }
            LOG_INFO("LinkWatcher: finished opening {} additional tabs", opened.size());
        }).detach();

        std::cout << "LinkWatcher: watch active. Saving OCR results to save-result/" << std::endl;
    }

    // Adds a session for a single watched tab (idempotent). connect() is
    // async, so we don't block here; the OCR loop only uses sessions whose
    // state is Monitoring (fully initialized).
    void attachWatchedTab(const CDPTargetId& cdpId, const std::string& wsUrl, const std::string& url) {
        if (cdpId.empty() || wsUrl.empty()) return;
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (sessions_.count(cdpId.value)) return;
        auto target = std::make_shared<PageSession>(
            TargetId("link_" + cdpId.value), cdpId, wsUrl, pool_);
        target->setErrorCallback([](const std::string& err, bool) {
            LOG_WARN("LinkWatcher session error: {}", err);
        });
        if (target->connect()) {
            target->enablePageEvents();
            sessions_[cdpId.value] = target;
            urlByCdp_[cdpId.value] = url;
            LOG_INFO("LinkWatcher: watching {} -> {}", cdpId.value, url);
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (ocrThread_.joinable()) {
            ocrThread_.request_stop();
            ocrThread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (auto& [id, session] : sessions_) {
                if (session) session->disconnect();
            }
            sessions_.clear();
            urlByCdp_.clear();
        }
    }

    size_t watchedCount() const {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        return sessions_.size();
    }

    size_t savedCount() const { return savedFiles_.load(); }

private:
    // One watched tab: resolved CDP target ready to attach.
    struct WatchedTab { std::string url; CDPTargetId cdpId; std::string wsUrl; };

    // Returns the links that are already open as Edge tabs.
    std::vector<WatchedTab> alreadyOpenTabs(const std::vector<std::string>& links) {
        std::vector<WatchedTab> out;
        std::set<std::string> seen;
        auto pages = engine_.discover();
        for (const auto& link : links) {
            if (seen.count(link)) continue;
            seen.insert(link);
            for (const auto& t : pages) {
                if (t.url.value == link) {
                    out.push_back({link, t.id, t.webSocketDebuggerUrl});
                    break;
                }
            }
        }
        return out;
    }

    // Connects a dedicated session to the base page and evaluates JS that
    // returns every unique <a href> as a JSON array string. Retries the
    // connection a few times because the first attempt is sometimes slow.
    std::vector<std::string> collectLinks(const EdgeTarget& baseTarget) {
        std::vector<std::string> links;
        const std::string script = R"(
            JSON.stringify((function() {
                var out = [];
                var seen = {};
                var nodes = document.querySelectorAll('a[href]');
                for (var i = 0; i < nodes.length; i++) {
                    var href = nodes[i].href;
                    if (!href) continue;
                    if (href.indexOf('javascript:') === 0) continue;
                    if (seen[href]) continue;
                    seen[href] = true;
                    out.push(href);
                }
                return out;
            })())
        )";

        for (int attempt = 0; attempt < 3; ++attempt) {
            auto session = std::make_shared<PageSession>(
                TargetId("__link_scan"), baseTarget.id, baseTarget.webSocketDebuggerUrl, pool_);
            session->connect();

            bool connected = false;
            for (int i = 0; i < 40; ++i) {
                // Wait for full session initialization (state becomes
                // Monitoring after DOM.enable/Page.enable are done), not just
                // the raw websocket â€” evaluating earlier races the setup.
                if (session->isConnected() &&
                    session->getState() == TargetState::Monitoring) {
                    connected = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (!connected) {
                LOG_WARN("LinkWatcher: base connect attempt {} failed", attempt + 1);
                continue;
            }

            auto res = session->executeJavaScriptSync(script, std::chrono::seconds(10));
            session->disconnect();

            if (!res) {
                LOG_WARN("LinkWatcher: link scan JS attempt {} returned nothing", attempt + 1);
                continue;
            }
            try {
                auto& v = (*res)["result"]["value"];
                if (v.is_string()) {
                    auto arr = nlohmann::json::parse(v.get<std::string>());
                    if (arr.is_array()) {
                        for (const auto& item : arr) {
                            if (item.is_string()) links.push_back(item.get<std::string>());
                        }
                    }
                    return links;
                }
            } catch (const std::exception& e) {
                LOG_WARN("LinkWatcher: link parse error: {}", e.what());
            }
        }
        return links;
    }

    // Resolve each link to an existing CDP target or ask Edge to open a new
    // tab. Returns (url, cdpId, wsUrl) triples ready to connect. Opening many
    // tabs is expensive and pointless, so we cap at maxTabs.
    static constexpr size_t kMaxTabs = 25;

    std::vector<WatchedTab> openAllTabs(const std::vector<std::string>& links) {
        std::vector<WatchedTab> out;
        std::set<std::string> seen;
        auto pages = engine_.discover();

        for (const auto& link : links) {
            if (out.size() >= kMaxTabs) {
                LOG_INFO("LinkWatcher: reached {} tab cap, skipping remaining links",
                         kMaxTabs);
                break;
            }
            // Only watch real http(s) pages.
            if (link.rfind("http://", 0) != 0 && link.rfind("https://", 0) != 0) {
                continue;
            }
            if (seen.count(link)) continue;
            seen.insert(link);

            bool found = false;
            for (const auto& t : pages) {
                if (t.url.value == link) {
                    out.push_back({link, t.id, t.webSocketDebuggerUrl});
                    found = true;
                    break;
                }
            }
            if (found) continue;

            // Not already open: ask the browser to open it (via /json/new).
            std::string cdpId;
            std::string wsUrl;
            if (openNewTab(link, cdpId, wsUrl)) {
                out.push_back({link, CDPTargetId(cdpId), wsUrl});
            }
        }
        return out;
    }

    // PUT to Edge's HTTP endpoint "http://127.0.0.1:<port>/json/new?<url>" to
    // create a new tab. Uses the cached active port (found once) so it never
    // re-probes or stalls.
    bool openNewTab(const std::string& url, std::string& outId, std::string& outWs) {
        if (debugPort_ == 0) {
            findDebugPort();
        }
        if (debugPort_ == 0) {
            LOG_WARN("LinkWatcher: no active Edge debugging port found");
            return false;
        }

        // URL-encode the link for the query string.
        std::string enc;
        for (char c : url) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
                c == '~' || c == '/' || c == ':' || c == '?' || c == '=' || c == '&' || c == '%' ||
                c == '+' || c == ',' || c == '@') {
                enc.push_back(c);
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                enc += buf;
            }
        }

        std::string raw;
        // CDP /json/new requires PUT (GET is deprecated/removed).
        if (httpRequest("127.0.0.1", debugPort_, "PUT", "/json/new?" + enc, 300, raw) && !raw.empty()) {
            try {
                auto j = nlohmann::json::parse(httpBodyOnly(raw));
                if (j.contains("id") && j.contains("webSocketDebuggerUrl")) {
                    outId = j["id"].get<std::string>();
                    outWs = j["webSocketDebuggerUrl"].get<std::string>();
                    LOG_INFO("LinkWatcher: opened new tab {} -> {}", url, outId);
                    return true;
                }
            } catch (const std::exception& e) {
                LOG_WARN("LinkWatcher: /json/new parse error: {}", e.what());
            }
        }
        LOG_WARN("LinkWatcher: could not open new tab for {}", url);
        return false;
    }

    // Probes the standard debugging ports once with a short timeout and caches
    // the one that answers /json/version.
    void findDebugPort() {
        const int ports[] = {9222, 9223, 9224, 9225, 9226, 9227, 9228, 9229, 9230};
        for (int p : ports) {
            std::string probe;
            if (httpRequest("127.0.0.1", p, "GET", "/json/version", 150, probe)) {
                debugPort_ = p;
                LOG_INFO("LinkWatcher: Edge debugging port is {}", p);
                return;
            }
        }
    }

    // Re-attaches watched sessions whose CDP connection died (e.g. Edge was
    // closed and relaunched). Re-resolves each URL to its fresh CDP target.
    void recoverDeadSessions() {
        // 1) Snapshot which sessions are dead.
        std::vector<std::pair<std::string, std::string>> dead;  // (cdpId, url)
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (auto& [id, session] : sessions_) {
                if (!session || !session->isConnected()) {
                    auto it = urlByCdp_.find(id);
                    if (it != urlByCdp_.end()) dead.emplace_back(id, it->second);
                }
            }
        }
        if (dead.empty()) return;

        // 2) Is CDP even reachable? If Edge is fully down, just retry next
        //    cycle (never relaunch or fabricate a profile here).
        if (!EdgeConnector::probe().cdpAvailable()) return;

        // 3) Re-discover once and re-resolve each dead URL.
        auto pages = engine_.discover();
        for (auto& [oldCdpId, url] : dead) {
            bool reattached = false;
            for (const auto& t : pages) {
                if (t.url.value == url) {
                    {
                        std::lock_guard<std::mutex> lock(sessionsMutex_);
                        auto it = sessions_.find(oldCdpId);
                        if (it != sessions_.end() && it->second &&
                            it->second->isConnected()) {
                            reattached = true;  // came back on its own
                            break;
                        }
                        // Swap in a fresh session under the same key.
                        auto fresh = std::make_shared<PageSession>(
                            TargetId("link_" + t.id.value), t.id,
                            t.webSocketDebuggerUrl, pool_);
                        fresh->setErrorCallback([](const std::string& err, bool) {
                            LOG_WARN("LinkWatcher session error: {}", err);
                        });
                        if (fresh->connect()) {
                            fresh->enablePageEvents();
                            sessions_[oldCdpId] = fresh;
                            urlByCdp_[oldCdpId] = url;
                            reattached = true;
                            LOG_INFO("LinkWatcher: reconnected {} -> {} (new CDP {})",
                                     oldCdpId, url, t.id.value);
                        }
                    }
                    break;
                }
            }
            if (!reattached) {
                LOG_WARN("LinkWatcher: could not re-resolve dead session {} ({})",
                         oldCdpId, url);
            }
        }
    }

    // Fast OCR loop: for each watched tab, screenshot -> OCR -> save. Runs as
    // fast as the browser + OCR allow; skips frames that produced no text and
    // caps the per-page rate to avoid flooding the disk.
    void ocrLoop(std::stop_token token) {
        auto lastSave = std::map<std::string, std::chrono::steady_clock::time_point>();
        int heartbeat = 0;
        auto lastRecoverCheck = std::chrono::steady_clock::now();
        while (!token.stop_requested() && running_) {
            // Recovery: if a watched session died (Edge restarted / tab closed),
            // re-resolve its URL via CDP and reattach. Bounded: once per ~5s.
            auto nowCheck = std::chrono::steady_clock::now();
            if (nowCheck - lastRecoverCheck > std::chrono::seconds(5)) {
                lastRecoverCheck = nowCheck;
                recoverDeadSessions();
            }

            std::vector<std::shared_ptr<PageSession>> snaps;
            {
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                for (auto& [id, session] : sessions_) snaps.push_back(session);
            }
            if (++heartbeat % 10 == 0) {
                LOG_INFO("LinkWatcher OCR heartbeat: {} watched sessions",
                         snaps.size());
            }
            for (auto& session : snaps) {
                if (!session || !session->isConnected()) continue;
                if (session->getState() != TargetState::Monitoring) continue;  // not fully initialized yet

                auto shot = session->captureScreenshot("png", 90, false);
                if (shot.empty() || !shot.contains("data")) {
                    LOG_DEBUG("LinkWatcher OCR: no screenshot data for {}",
                              session->getTargetId().value);
                    continue;
                }

                try {
                    CapturedFrame frame;
                    frame.targetId = session->getTargetId();
                    frame.cdpTargetId = session->getCDPTargetId();
                    frame.timestamp = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    frame.status = CaptureStatus::Success;
                    frame.pixels = decodeBase64Image(shot["data"].get<std::string>());
                    LOG_DEBUG("LinkWatcher OCR: got {} bytes screenshot for {}",
                              frame.pixels.size(), session->getTargetId().value);

                    // Engine availability gate: only report real OCR text.
                    if (!ocrEngine_ || !ocrEngine_->available()) {
                        LOG_WARN("LinkWatcher OCR: engine unavailable, skipping frame for {}",
                                 session->getTargetId().value);
                        continue;
                    }
                    std::vector<uint8_t> bgra;
                    int imgW = 0, imgH = 0;
                    if (!OcrEngine::decodePng(frame.pixels, bgra, imgW, imgH)) {
                        LOG_DEBUG("LinkWatcher OCR: PNG decode failed for {}",
                                  session->getTargetId().value);
                        continue;
                    }
                    OcrPageResult page = ocrEngine_->recognize(bgra.data(), imgW, imgH);
                    LOG_DEBUG("LinkWatcher OCR: {} lines from {}x{} for {} ({:.1f}ms)",
                              page.lines.size(), imgW, imgH, session->getTargetId().value,
                              ocrEngine_->lastLatencyMs());
                    if (page.lines.empty()) continue;

                    // Confidence gating: drop garbage lines (short, low-conf)
                    // and require a minimum mean confidence to save anything.
                    std::ostringstream cleanText;
                    float confSum = 0;
                    int confN = 0;
                    for (const auto& l : page.lines) {
                        if (l.confidence < 0.35f) continue;          // junk line
                        if (l.text.size() < 2 && l.text.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == std::string::npos) continue;
                        cleanText << l.text << "\n";
                        confSum += l.confidence;
                        confN++;
                    }
                    std::string clean = cleanText.str();
                    if (!clean.empty() && clean.back() == '\n') clean.pop_back();
                    if (clean.size() < 4) continue;                  // nothing meaningful
                    float meanConf = confN > 0 ? confSum / confN : 0.0f;
                    if (meanConf < 0.35f) continue;                 // overall garbage

                    OCRResult result;
                    result.targetId = session->getTargetId();
                    result.text = clean;
                    result.confidence = meanConf;
                    result.timestamp = frame.timestamp;

                    if (!result.text.empty()) {
                        // Rate-limit saves to at most one per ~1s per page so
                        // identical frames don't hammer the disk.
                        auto now = std::chrono::steady_clock::now();
                        std::string name = session->getTargetId().value;
                        auto it = lastSave.find(name);
                        if (it != lastSave.end() &&
                            now - it->second < std::chrono::seconds(1)) {
                            continue;
                        }
                        lastSave[name] = now;
                        std::string file = saver_.save(session->getTargetId(), name,
                                                       result.text, result.confidence);
                        if (!file.empty()) {
                            savedFiles_++;
                            // Feed the GUI live: terminal line + report row.
                            std::ostringstream line;
                            line << "OCR saved " << file << " ("
                                 << result.text.size() << " chars, conf="
                                 << std::fixed << std::setprecision(2)
                                 << result.confidence << ")";
                            live::pushLog(line.str());
                            live::updatePage(name, name, result.text, file, result.confidence);
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("LinkWatcher OCR error: {}", e.what());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    MonitorEngine& engine_;
    std::string baseUrl_;
    std::shared_ptr<ThreadPool> pool_;
    int debugPort_ = 0;
    std::shared_ptr<OcrEngine> ocrEngine_;  // real PP-OCRv3 engine (loaded once)

    mutable std::mutex sessionsMutex_;
    std::map<std::string, std::shared_ptr<PageSession>> sessions_;  // cdpId -> session
    std::map<std::string, std::string> urlByCdp_;                   // cdpId -> url (for reconnect)

    std::atomic<bool> running_;
    std::jthread ocrThread_;
    ResultSaver saver_;
    std::atomic<size_t> savedFiles_{0};
};

// ============================================================================
// LiveMultiWatcher â€” NO fixed targets. Watches every currently-open Edge tab
// (and, when a base URL is supplied, the links reachable from that page) with
// a real OCR pass per tab, feeding the GUI terminal + per-page report list.
//
// CDP auto-ensure: if Edge is already CDP-live (custom profile) we use it; if
// it is running on the default profile (where Chromium >= 136 ignores
// --remote-debugging-port), we relaunch it once with a NON-DEFAULT profile dir
// so OCR actually works. This is the user-requested "no target locking, just
// watch whatever I have open" path. The original exact-target flow is separate
// and untouched.
// ============================================================================

class LiveMultiWatcher {
public:
    LiveMultiWatcher(MonitorEngine& engine, std::vector<std::string> urls,
                     std::shared_ptr<ThreadPool> pool, std::string outRoot = "save-result")
        : engine_(engine)
        , urls_(std::move(urls))
        , pool_(std::move(pool))
        , outRoot_(std::move(outRoot))
        , saver_(outRoot_)
        , running_(false) {
    }

    ~LiveMultiWatcher() { stop(); }

    // Returns a CDP-live port, OR launches a dedicated Edge instance on a
    // NON-DEFAULT profile so CDP actually binds (Chromium >= 136 refuses to
    // debug the default profile). Returns 0 when no CDP can be made available.
    static int ensureCdpForWatch() {
        EdgeProbeResult probe = EdgeConnector::ensureDedicatedCdpInstance();
        if (!probe.cdpAvailable()) {
            for (const auto& r : probe.reasons) {
                live::pushLog("CDP ensure failed: " + r);
            }
            LOG_ERROR("LiveMultiWatcher: CDP ensure failed");
            return 0;
        }
        return probe.cdpPort;
    }

    void start() {
        if (running_.exchange(true)) return;

        std::cout << "LiveMultiWatcher: ensuring CDP..." << std::endl;
        int port = ensureCdpForWatch();
        if (port == 0) {
            std::cout << "LiveMultiWatcher: CDP unavailable." << std::endl;
            running_ = false;
            live::setWatching(false);
            return;
        }
        debugPort_ = port;
        LOG_INFO("LiveMultiWatcher: CDP live on port {}", port);

        // 0) Create the output root for the continuous live report.
        std::error_code ec;
        std::filesystem::create_directories(outRoot_, ec);
        reportPath_ = (std::filesystem::path(outRoot_) / "LIVE_REPORT.txt").string();

        // 1) Discover ALL currently-open page tabs (no target locking) and
        //    attach to every one of them.
        auto pages = engine_.discover();
        std::vector<EdgeTarget> attachable;
        for (const auto& t : pages) {
            if (t.type == "page") attachable.push_back(t);
        }

        // 2) Open any explicitly-given URLs that are not open yet (each becomes
        //    its own watched tab => rugged multi-link OCR).
        for (const auto& url : urls_) {
            bool alreadyOpen = false;
            for (const auto& t : attachable) {
                if (UrlUtils::matches(url, t.url.value, TargetMatchRule::Exact)) {
                    alreadyOpen = true;
                    break;
                }
            }
            if (!alreadyOpen) {
                OpenedTab ot = openCdpTab(port, url);
                if (!ot.id.empty()) {
                    live::pushLog("Opened " + url + " (new tab) for OCR.");
                }
            }
        }

        // 3) Re-discover now that URLs were opened, then attach to every tab.
        pages = engine_.discover();
        attachable.clear();
        for (const auto& t : pages) {
            if (t.type == "page") attachable.push_back(t);
        }
        if (attachable.empty()) {
            live::pushLog("No open Edge tabs to watch. Open pages in Edge and click Watch again.");
            std::cerr << "LiveMultiWatcher: no open page tabs." << std::endl;
            running_ = false;
            live::setWatching(false);
            return;
        }
        for (const auto& t : attachable) {
            attachWatchedTab(t.id, t.webSocketDebuggerUrl, t.url.value);
        }
        std::cout << "LiveMultiWatcher: " << attachable.size() << " open tabs." << std::endl;
        live::pushLog("Live watch: attached to " +
                      std::to_string(watchedCount()) + " tab(s).");

        // 4) Load the real OCR engine once.
        ocrEngine_ = std::make_shared<OcrEngine>();
        if (ocrEngine_->loadModels("models")) {
            LOG_INFO("LiveMultiWatcher: real OCR engine loaded (PP-OCRv3)");
        } else {
            LOG_ERROR("LiveMultiWatcher: real OCR engine unavailable: {}", ocrEngine_->lastError());
        }

        // 5) Continuous live report: a marker + summary line written up front
        //    (rows are appended by each OCR save below).
        {
            std::ofstream rep(reportPath_, std::ios::app);
            if (rep) {
                rep << "=== Edge Monitor LIVE REPORT ===\n"
                    << "Started: " << timestampStr() << "\n"
                    << "Output : " << outRoot_ << "\n"
                    << "Tabs   : " << watchedCount() << "\n"
                    << "----------------------------------------\n";
            }
        }

        ocrThread_ = std::jthread([this](std::stop_token token) { ocrLoop(token); });
        std::cout << "LiveMultiWatcher: watch active. Saving OCR results to "
                  << outRoot_ << std::endl;
    }

    void attachWatchedTab(const CDPTargetId& cdpId, const std::string& wsUrl, const std::string& url) {
        if (cdpId.empty() || wsUrl.empty()) return;
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (sessions_.count(cdpId.value)) return;
        auto target = std::make_shared<PageSession>(
            TargetId("live_" + cdpId.value), cdpId, wsUrl, pool_);
        target->setErrorCallback([](const std::string& err, bool) {
            LOG_WARN("LiveMultiWatcher session error: {}", err);
        });
        if (target->connect()) {
            target->enablePageEvents();
            sessions_[cdpId.value] = target;
            urlByCdp_[cdpId.value] = url;
            LOG_INFO("LiveMultiWatcher: watching {} -> {}", cdpId.value, url);
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (ocrThread_.joinable()) {
            ocrThread_.request_stop();
            ocrThread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (auto& [id, session] : sessions_) {
                if (session) session->disconnect();
            }
            sessions_.clear();
            urlByCdp_.clear();
        }
    }

    size_t watchedCount() const {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        return sessions_.size();
    }

    size_t savedCount() const { return savedFiles_.load(); }

private:
    void recoverDeadSessions() {
        std::vector<std::pair<std::string, std::string>> dead;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (auto& [id, session] : sessions_) {
                if (!session || !session->isConnected()) {
                    auto it = urlByCdp_.find(id);
                    if (it != urlByCdp_.end()) dead.emplace_back(id, it->second);
                }
            }
        }
        if (dead.empty()) return;
        if (!EdgeConnector::probe().cdpAvailable()) return;
        auto pages = engine_.discover();
        for (auto& [oldCdpId, url] : dead) {
            bool reattached = false;
            for (const auto& t : pages) {
                if (t.url.value == url) {
                    {
                        std::lock_guard<std::mutex> lock(sessionsMutex_);
                        auto it = sessions_.find(oldCdpId);
                        if (it != sessions_.end() && it->second &&
                            it->second->isConnected()) {
                            reattached = true;
                            break;
                        }
                        auto fresh = std::make_shared<PageSession>(
                            TargetId("live_" + t.id.value), t.id,
                            t.webSocketDebuggerUrl, pool_);
                        fresh->setErrorCallback([](const std::string& err, bool) {
                            LOG_WARN("LiveMultiWatcher session error: {}", err);
                        });
                        if (fresh->connect()) {
                            fresh->enablePageEvents();
                            sessions_[oldCdpId] = fresh;
                            urlByCdp_[oldCdpId] = url;
                            reattached = true;
                        }
                    }
                    break;
                }
            }
            if (!reattached) {
                LOG_WARN("LiveMultiWatcher: could not re-resolve dead session {} ({})", oldCdpId, url);
            }
        }
    }

    void ocrLoop(std::stop_token token) {
        auto lastSave = std::map<std::string, std::chrono::steady_clock::time_point>();
        auto lastRecoverCheck = std::chrono::steady_clock::now();
        auto lastNewTabScan = std::chrono::steady_clock::now();
        while (!token.stop_requested() && running_) {
            auto nowCheck = std::chrono::steady_clock::now();
            if (nowCheck - lastRecoverCheck > std::chrono::seconds(5)) {
                lastRecoverCheck = nowCheck;
                recoverDeadSessions();
            }
            // Rugged: pick up tabs the user opens AFTER start (every ~4s).
            if (nowCheck - lastNewTabScan > std::chrono::seconds(4)) {
                lastNewTabScan = nowCheck;
                auto fresh = engine_.discover();
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                for (const auto& t : fresh) {
                    if (t.type != "page") continue;
                    if (sessions_.count(t.id.value)) continue;
                    if (t.id.empty() || t.webSocketDebuggerUrl.empty()) continue;
                    auto ns = std::make_shared<PageSession>(
                        TargetId("live_" + t.id.value), t.id,
                        t.webSocketDebuggerUrl, pool_);
                    ns->setErrorCallback([](const std::string& err, bool) {
                        LOG_WARN("LiveMultiWatcher session error: {}", err);
                    });
                    if (ns->connect()) {
                        ns->enablePageEvents();
                        sessions_[t.id.value] = ns;
                        urlByCdp_[t.id.value] = t.url.value;
                        live::pushLog("Auto-attached new tab: " + t.url.value);
                    }
                }
            }
            std::vector<std::shared_ptr<PageSession>> snaps;
            {
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                for (auto& [id, session] : sessions_) snaps.push_back(session);
            }
            for (auto& session : snaps) {
                if (!session || !session->isConnected()) continue;
                if (session->getState() != TargetState::Monitoring) continue;
                auto shot = session->captureScreenshot("png", 90, false);
                if (shot.empty() || !shot.contains("data")) continue;
                try {
                    CapturedFrame frame;
                    frame.targetId = session->getTargetId();
                    frame.cdpTargetId = session->getCDPTargetId();
                    frame.timestamp = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    frame.status = CaptureStatus::Success;
                    frame.pixels = decodeBase64Image(shot["data"].get<std::string>());
                    if (!ocrEngine_ || !ocrEngine_->available()) continue;
                    std::vector<uint8_t> bgra;
                    int imgW = 0, imgH = 0;
                    if (!OcrEngine::decodePng(frame.pixels, bgra, imgW, imgH)) continue;
                    OcrPageResult page = ocrEngine_->recognize(bgra.data(), imgW, imgH);
                    if (page.lines.empty()) continue;
                    std::ostringstream cleanText;
                    float confSum = 0;
                    int confN = 0;
                    for (const auto& l : page.lines) {
                        if (l.confidence < 0.35f) continue;
                        cleanText << l.text << "\n";
                        confSum += l.confidence;
                        confN++;
                    }
                    std::string clean = cleanText.str();
                    if (!clean.empty() && clean.back() == '\n') clean.pop_back();
                    if (clean.size() < 4) continue;
                    float meanConf = confN > 0 ? confSum / confN : 0.0f;
                    if (meanConf < 0.35f) continue;
                    auto now = std::chrono::steady_clock::now();
                    std::string name = session->getTargetId().value;
                    auto it = lastSave.find(name);
                    if (it != lastSave.end() &&
                        now - it->second < std::chrono::seconds(1)) continue;
                    lastSave[name] = now;
                    std::string file = saver_.save(session->getTargetId(), name, clean, meanConf);
                    if (!file.empty()) {
                        savedFiles_++;
                        live::pushLog("OCR saved " + file + " (" +
                                      std::to_string(clean.size()) + " chars, conf=" +
                                      std::to_string(meanConf) + ")");
                        live::updatePage(name, name, clean, file, meanConf);
                        // Continuous live report: append a timestamped row per
                        // page so the file grows in real time.
                        std::string cdpId = session->getCDPTargetId().value;
                        std::string tabUrl;
                        {
                            std::lock_guard<std::mutex> lock(sessionsMutex_);
                            auto it = urlByCdp_.find(cdpId);
                            if (it != urlByCdp_.end()) tabUrl = it->second;
                        }
                        std::ofstream rep(reportPath_, std::ios::app);
                        if (rep) {
                            rep << "[" << timestampStr() << "] TAB " << cdpId
                                << " URL=" << (tabUrl.empty() ? name : tabUrl)
                                << "\n  conf=" << std::fixed << std::setprecision(2)
                                << meanConf << " chars=" << clean.size()
                                << " file=" << file << "\n"
                                << "  text: " << clean.substr(0, 400) << "\n";
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("LiveMultiWatcher OCR error: {}", e.what());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    MonitorEngine& engine_;
    std::vector<std::string> urls_;
    std::shared_ptr<ThreadPool> pool_;
    std::string outRoot_;
    std::string reportPath_;
    int debugPort_ = 0;
    std::shared_ptr<OcrEngine> ocrEngine_;

    mutable std::mutex sessionsMutex_;
    std::map<std::string, std::shared_ptr<PageSession>> sessions_;
    std::map<std::string, std::string> urlByCdp_;

    std::atomic<bool> running_;
    std::jthread ocrThread_;
    ResultSaver saver_;
    std::atomic<size_t> savedFiles_{0};
};

// ============================================================================
// Live shared state â€” log terminal + per-page report, read by the GUI timer
// ============================================================================

namespace live {

struct PageReport {
    std::string url;          // original URL / tab identity
    std::string lastText;     // last OCR text (truncated for display)
    std::string lastFile;     // last saved file path
    std::string lastTime;     // HH:MM:SS of last save
    size_t saves = 0;
    size_t chars = 0;
    float confidence = 0.0f;
};

struct State {
    // Terminal: ring buffer of recent log lines.
    std::mutex logMutex;
    std::vector<std::string> logLines;
    static constexpr size_t kMaxLogLines = 2000;

    // Report: per-page key -> latest OCR info.
    std::mutex repMutex;
    std::map<std::string, PageReport> pages;

    // The active watcher (owned here so the GUI can start/stop it).
    std::shared_ptr<class LinkWatcher> watcher;
    std::atomic<bool> watching{false};
};

inline State& state() {
    static State s;
    return s;
}

// Log sink: appended from any thread (Logger, watcher, GUI). Terminal shows
// these lines. Dedupes consecutive identical lines (busy loops would flood).
inline void pushLog(const std::string& line) {
    auto& st = state();
    std::lock_guard<std::mutex> lock(st.logMutex);
    if (!st.logLines.empty() && st.logLines.back() == line) {
        return;
    }
    st.logLines.push_back(line);
    if (st.logLines.size() > State::kMaxLogLines) {
        st.logLines.erase(st.logLines.begin(),
                          st.logLines.begin() + (st.logLines.size() - State::kMaxLogLines));
    }
}

inline std::vector<std::string> drainLog() {
    auto& st = state();
    std::lock_guard<std::mutex> lock(st.logMutex);
    return st.logLines;
}

// Sets the shared "is any watch active" flag (GUI status bar / mode toggling).
inline void setWatching(bool on) {
    state().watching.store(on);
}

inline void updatePage(const std::string& key, const std::string& url,
                       const std::string& text, const std::string& file,
                       float confidence) {
    auto& st = state();
    std::lock_guard<std::mutex> lock(st.repMutex);
    auto& p = st.pages[key];
    p.url = url;
    p.lastText = text;
    p.lastFile = file;
    p.saves++;
    p.chars += static_cast<size_t>(text.size());
    p.confidence = confidence;
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    p.lastTime = buf;
}

inline std::vector<std::pair<std::string, PageReport>> allPages() {
    auto& st = state();
    std::lock_guard<std::mutex> lock(st.repMutex);
    std::vector<std::pair<std::string, PageReport>> out;
    for (const auto& [k, v] : st.pages) out.emplace_back(k, v);
    return out;
}

inline size_t totalSaves() {
    auto& st = state();
    std::lock_guard<std::mutex> lock(st.repMutex);
    size_t n = 0;
    for (const auto& [k, v] : st.pages) n += v.saves;
    return n;
}

} // namespace live

} // namespace

// Opens a URL as a new tab in the CDP browser at cdpPort (PUT /json/new) and
// returns its CDP id + websocket URL. Defined here (after the anon-namespace
// HTTP helpers, which stay visible via the implicit using-directive) so
// startExactWatch can open a requested URL that is not open yet.
OpenedTab openCdpTab(int cdpPort, const std::string& url) {
    OpenedTab out;
    if (cdpPort <= 0 || url.empty()) return out;

    std::string enc;
    for (char c : url) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/' || c == ':' || c == '?' || c == '=' || c == '&' || c == '%' ||
            c == '+' || c == ',' || c == '@') {
            enc.push_back(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            enc += buf;
        }
    }

    std::string body;
    if (httpRequest("127.0.0.1", cdpPort, "PUT", "/json/new?" + enc, 1000, body) &&
        !body.empty()) {
        try {
            auto j = nlohmann::json::parse(httpBodyOnly(body));
            if (j.contains("id") && j.contains("webSocketDebuggerUrl")) {
                out.id = j["id"].get<std::string>();
                out.webSocketUrl = j["webSocketDebuggerUrl"].get<std::string>();
            }
        } catch (const std::exception&) {
        }
    }
    return out;
}

// ============================================================================
// Win32 GUI â€” single all-in-one window: URL add + Watch, live report (left),
// live terminal (right), status bar.
// ============================================================================

#ifdef _WIN32

struct GuiState {
    // Controls
    HWND hUrl = nullptr;     // URL(s) edit
    HWND hWatch = nullptr;   // "Watch URL" button
    HWND hLive = nullptr;    // "Watch these URLs" button (multi-tab, no fixed targets)
    HWND hStop = nullptr;    // "Stop" button
    HWND hOut = nullptr;     // output folder edit (live report + saves)
    HWND hStatus = nullptr;  // status bar
    HWND hTarget = nullptr;  // target info line (Target / URL / CDP)
    HWND hReport = nullptr;  // report list (per-page OCR rows)
    HWND hTerminal = nullptr;// terminal (log lines)
    HWND hDetail = nullptr;  // selected page detail / last OCR text

    std::atomic<bool> running{true};
    MonitorEngine* engine = nullptr;
    // The active EXACT-TARGET monitor (primary watch flow).
    std::shared_ptr<ExactTargetMonitor> monitor;
    // The active NO-FIXED-TARGET multi-page watcher (GUI "Live Watch All Tabs").
    std::shared_ptr<class LiveMultiWatcher> liveWatcher;
    std::mutex mutex;   // guards monitor + liveWatcher

    // Live counters shown in the status bar (moved only by real OCR saves).
    std::atomic<size_t> pagesSaved{0};
    std::atomic<size_t> ocrAccepted{0};
    std::atomic<bool> locked{false};
    std::atomic<unsigned> watchGen{0};  // bumped on each start/stop
};

static GuiState g_gui;

// ---- Report / terminal rendering -------------------------------------------

static void renderReport() {
    auto pages = live::allPages();
    SendMessageA(g_gui.hReport, LB_RESETCONTENT, 0, 0);
    for (const auto& [key, p] : pages) {
        std::ostringstream oss;
        oss << "[" << key.substr(0, 20) << "]  saves=" << p.saves
            << "  chars=" << p.chars
            << "  conf=" << std::fixed << std::setprecision(2) << p.confidence
            << "  last=" << p.lastTime;
        SendMessageA(g_gui.hReport, LB_ADDSTRING, 0, (LPARAM)oss.str().c_str());
    }
}

static void renderTerminal() {
    auto lines = live::drainLog();
    std::ostringstream oss;
    // Show the last ~200 lines so the pane stays snappy.
    size_t start = lines.size() > 200 ? lines.size() - 200 : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        oss << lines[i] << "\r\n";
    }
    SetWindowTextA(g_gui.hTerminal, oss.str().c_str());
    // Auto-scroll to the bottom.
    SendMessageA(g_gui.hTerminal, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageA(g_gui.hTerminal, EM_SCROLLCARET, 0, 0);
}

// Every ~500ms tick: update status bar, target line, report list, terminal.
static void guiRefresh() {
    if (!g_gui.engine) return;

    auto stats = g_gui.engine->stats();
    std::ostringstream oss;
    oss << "Edge Monitor  |  Watching: " << (live::state().watching.load() ? "YES" : "no")
        << "  |  Pages saved: " << g_gui.pagesSaved.load()
        << "  |  OCR accepted: " << g_gui.ocrAccepted.load()
        << "  |  Vectors: " << g_gui.engine->vectorCount()
        << "  |  Targets: " << stats.totalLocked;
    SetWindowTextA(g_gui.hStatus, oss.str().c_str());

    // Visible target line: the ACTUAL selected/locked page, or live-watch
    // summary when the no-fixed-target mode is active.
    {
        std::lock_guard<std::mutex> lock(g_gui.mutex);
        if (g_gui.liveWatcher && live::state().watching.load()) {
            std::ostringstream t;
            t << "Live watch: " << g_gui.liveWatcher->watchedCount()
              << " tab(s) — all open pages OCR'd, no target lock\r\n"
              << "Saved: " << g_gui.liveWatcher->savedCount()
              << "  |  Mode: no-fixed-target";
            if (g_gui.hTarget) SetWindowTextA(g_gui.hTarget, t.str().c_str());
        } else if (g_gui.monitor && g_gui.monitor->isRunning()) {
            std::ostringstream t;
            t << "Target: " << g_gui.monitor->currentTitle()
              << "\r\nURL: " << g_gui.monitor->currentUrl()
              << "\r\nCDP: " << g_gui.monitor->currentCdpTargetId();
            if (g_gui.hTarget) SetWindowTextA(g_gui.hTarget, t.str().c_str());
        } else if (g_gui.hTarget) {
            SetWindowTextA(g_gui.hTarget,
                           "Target: (none)\r\nURL: (none)\r\nCDP: (none)");
        }
    }

    renderReport();
    renderTerminal();
}

// ---- Watch control (EXACT-TARGET primary flow) -----------------------------

static void startWatchFromGui() {
    char buf[2048] = {0};
    GetWindowTextA(g_gui.hUrl, buf, sizeof(buf));
    std::string url = normalizeUrl(buf);
    if (url.empty()) {
        live::pushLog("Enter a URL to watch first.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_gui.mutex);
        if (g_gui.monitor && live::state().watching.load()) {
            live::pushLog("Already watching. Stop first, or enter a new URL.");
            return;
        }
        // Stop any previous monitor (live watcher too, so modes don't stack).
        if (g_gui.liveWatcher) {
            g_gui.liveWatcher->stop();
            g_gui.liveWatcher.reset();
        }
        if (g_gui.monitor) {
            g_gui.monitor->stop();
            g_gui.monitor.reset();
        }
        g_gui.pagesSaved = 0;
        g_gui.ocrAccepted = 0;
        g_gui.locked = false;
        g_gui.watchGen++;
    }

    live::pushLog("requested_url=" + url);
    live::state().watching = true;

    // Start off the GUI thread so the window stays responsive. The exact
    // resolver picks ONLY the tab whose URL matches — never the first page.
    std::thread([url, gen = g_gui.watchGen.load()]() {
        auto logCb = [](const std::string& line) { live::pushLog(line); };
        auto monitor = g_gui.engine->startExactWatch(url, false, logCb);
        {
            std::lock_guard<std::mutex> lock(g_gui.mutex);
            // If the user clicked Stop (or Watch again) while resolution was
            // in flight, discard this late result.
            if (gen != g_gui.watchGen.load()) {
                if (monitor) monitor->stop();
                return;
            }
            if (!monitor) {
                // TARGET_NOT_FOUND / AUTHENTICATED_SESSION_UNAVAILABLE: stay NO.
                live::state().watching = false;
                g_gui.locked = false;
                live::pushLog("Watching: NO — target not locked.");
                return;
            }
            g_gui.monitor = monitor;
            g_gui.locked = true;
            std::string lockedUrl = monitor->currentRuleUrl();
            // Counters move only on real saved OCR results from THIS target.
            monitor->setSavedOcrCallback(
                [](const std::string& cdpId, const std::string& pageUrl,
                   const std::string& title, const std::string& text,
                   float confidence) {
                    g_gui.pagesSaved++;
                    g_gui.ocrAccepted++;
                    live::updatePage(cdpId, pageUrl, text, cdpId + "/" + pageUrl,
                                     confidence);
                    live::pushLog("OCR saved: " + std::to_string(text.size()) +
                                  " chars conf=" +
                                  std::to_string(confidence) + " cdp=" + cdpId);
                });
            live::pushLog("Watching: YES — " + lockedUrl);
        }
    }).detach();
}

// ---- Watch control (NO-FIXED-TARGET live flow: every open tab, no locking) --

static void startLiveWatchFromGui() {
    char buf[8192] = {0};
    GetWindowTextA(g_gui.hUrl, buf, sizeof(buf));
    std::vector<std::string> urls = splitUrls(buf);

    char outBuf[1024] = {0};
    GetWindowTextA(g_gui.hOut, outBuf, sizeof(outBuf));
    std::string outRoot = outBuf;
    // Trim.
    size_t b = outRoot.find_first_not_of(" \t\r\n");
    size_t e = outRoot.find_last_not_of(" \t\r\n");
    outRoot = (b == std::string::npos) ? std::string("save-result")
                                       : outRoot.substr(b, e - b + 1);
    if (outRoot.empty()) outRoot = "save-result";

    if (urls.empty()) {
        live::pushLog("Enter one or more URLs (space/comma/newline separated) to watch.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_gui.mutex);
        if (live::state().watching.load()) {
            live::pushLog("Already watching. Stop first.");
            return;
        }
        // Stop any previous exact-target monitor.
        if (g_gui.monitor) {
            g_gui.monitor->stop();
            g_gui.monitor.reset();
        }
        if (g_gui.liveWatcher) {
            g_gui.liveWatcher->stop();
            g_gui.liveWatcher.reset();
        }
        g_gui.pagesSaved = 0;
        g_gui.ocrAccepted = 0;
        g_gui.locked = false;
        g_gui.watchGen++;
    }

    live::pushLog("Live multi-tab watch: OCR on " + std::to_string(urls.size()) +
                  " URL(s) + all open tabs (no fixed targets). Output: " + outRoot);
    live::state().watching = true;

    // Start off the GUI thread so the window stays responsive. The live
    // watcher auto-ensures CDP (dedicated non-default profile when needed),
    // opens any given URLs, and OCRs every open tab + URLs you add later.
    std::thread([urls, outRoot]() {
        auto watcher = std::make_shared<LiveMultiWatcher>(
            *g_gui.engine, urls, g_gui.engine->pool(), outRoot);
        watcher->start();
        {
            std::lock_guard<std::mutex> lock(g_gui.mutex);
            if (watcher->watchedCount() == 0) {
                live::state().watching = false;
                g_gui.locked = false;
                live::pushLog("Live watch: no tabs to OCR.");
                return;
            }
            g_gui.liveWatcher = watcher;
            g_gui.locked = true;
            live::pushLog("Live watch active — OCR on " +
                          std::to_string(watcher->watchedCount()) +
                          " tab(s). Saving to " + outRoot +
                          "\\LIVE_REPORT.txt (continuous). New tabs are picked up automatically.");
        }
    }).detach();
}

static void stopWatchFromGui() {
    std::lock_guard<std::mutex> lock(g_gui.mutex);
    live::state().watching = false;
    g_gui.locked = false;
    g_gui.watchGen++;
    if (g_gui.liveWatcher) {
        g_gui.liveWatcher->stop();
        g_gui.liveWatcher.reset();
    }
    if (g_gui.monitor) {
        g_gui.monitor->stop();
        g_gui.monitor.reset();
    }
    live::pushLog("Watch stopped.");
}

// ---- Window proc -----------------------------------------------------------

static LRESULT CALLBACK guiWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Top row: URL(s) edit + buttons.
            g_gui.hUrl = CreateWindowA("EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                10, 10, 380, 26, hWnd, (HMENU)1, nullptr, nullptr);
            g_gui.hWatch = CreateWindowA("BUTTON", "Watch URL",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                398, 10, 90, 26, hWnd, (HMENU)2, nullptr, nullptr);
            g_gui.hLive = CreateWindowA("BUTTON", "Watch URLs",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                496, 10, 96, 26, hWnd, (HMENU)9, nullptr, nullptr);
            g_gui.hStop = CreateWindowA("BUTTON", "Stop",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                600, 10, 70, 26, hWnd, (HMENU)3, nullptr, nullptr);

            // Second row: output folder + label.
            CreateWindowA("STATIC", "Output folder:",
                WS_CHILD | WS_VISIBLE, 10, 42, 90, 18, hWnd, (HMENU)10, nullptr, nullptr);
            g_gui.hOut = CreateWindowA("EDIT", "save-result",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                104, 40, 300, 22, hWnd, (HMENU)11, nullptr, nullptr);

            // Status bar.
            g_gui.hStatus = CreateWindowA("STATIC", "",
                WS_CHILD | WS_VISIBLE,
                10, 68, 670, 18, hWnd, (HMENU)4, nullptr, nullptr);

            // Target line: the ACTUAL locked page (Target / URL / CDP).
            g_gui.hTarget = CreateWindowA("STATIC",
                "Target: (none)\r\nURL: (none)\r\nCDP: (none)",
                WS_CHILD | WS_VISIBLE,
                10, 88, 670, 52, hWnd, (HMENU)8, nullptr, nullptr);

            // Report list (left, ~40% width): per-page OCR rows.
            g_gui.hReport = CreateWindowA("LISTBOX", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY,
                10, 146, 270, 240, hWnd, (HMENU)5, nullptr, nullptr);

            // Detail pane under the report: last OCR text of selected page.
            g_gui.hDetail = CreateWindowA("EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                10, 392, 270, 180, hWnd, (HMENU)6, nullptr, nullptr);

            // Terminal (right): live log stream.
            g_gui.hTerminal = CreateWindowA("EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                290, 146, 390, 426, hWnd, (HMENU)7, nullptr, nullptr);

            // Seed terminal with a welcome line.
            live::pushLog("Edge Monitor GUI ready. Paste one or more URLs (space/comma/newline "
                          "separated) and click Watch URLs to OCR all of them + every open tab "
                          "(no fixed targets). Set the Output folder; results stream to "
                          "LIVE_REPORT.txt continuously.");
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 2) {          // Watch URL
                startWatchFromGui();
            } else if (id == 9) {   // Live Watch All Tabs
                startLiveWatchFromGui();
            } else if (id == 3) {   // Stop
                stopWatchFromGui();
            } else if (id == 5 && HIWORD(wParam) == LBN_SELCHANGE) {
                // User selected a report row -> show its last text in detail.
                int sel = (int)SendMessageA(g_gui.hReport, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    auto pages = live::allPages();
                    if (sel < (int)pages.size()) {
                        const auto& [key, p] = pages[sel];
                        std::ostringstream oss;
                        oss << "Page: " << key << "\r\n"
                            << "Saves: " << p.saves << "  chars: " << p.chars
                            << "  conf: " << std::fixed << std::setprecision(2) << p.confidence
                            << "\r\nLast: " << p.lastTime << "\r\nFile: " << p.lastFile
                            << "\r\n\r\n" << p.lastText;
                        SetWindowTextA(g_gui.hDetail, oss.str().c_str());
                    }
                }
            }
            break;
        }
        case WM_TIMER: {
            guiRefresh();
            break;
        }
        case WM_DESTROY: {
            KillTimer(hWnd, 1);
            stopWatchFromGui();
            g_gui.running = false;
            PostQuitMessage(0);
            break;
        }
        default:
            return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
    return 0;
}

static void guiThread(MonitorEngine* engine) {
    g_gui.engine = engine;
    HINSTANCE hInst = GetModuleHandleA(nullptr);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = guiWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "EdgeMonitorGUI";
    RegisterClassA(&wc);

    HWND hWnd = CreateWindowA("EdgeMonitorGUI", "Edge Monitor - Live Watch",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 700, 650,
                              nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return;

    // Fast refresh (500ms) so the terminal + report feel live.
    SetTimer(hWnd, 1, 500, nullptr);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    g_gui.running = false;
}

#endif // _WIN32

// ============================================================================
// Entry point
// ============================================================================

static std::unique_ptr<MonitorEngine> g_engine;
static std::atomic<bool> g_interrupted{false};

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        LOG_INFO("Received signal {}, shutting down...", signal);
        g_interrupted = true;
        if (g_engine) {
            g_engine->stop();
        }
    }
}

} // namespace edgemon

using namespace edgemon;

// Single-instance guard: only one monitor may run at a time (shared Edge CDP,
// shared SQLite db, shared save-result dir). A second launch with the same
// intent shows a message and exits instead of colliding. --force bypasses.
static HANDLE g_singleInstanceMutex = nullptr;

static bool acquireSingleInstance(const std::string& cmd) {
    if (cmd == "--force") return true;
    // One-off CLI reads (discover/targets/status/search) are stateless and
    // safe to run alongside a live monitor.
    static const std::string readOnly[] = {"discover", "targets", "status", "search", "help", "--help"};
    for (const auto& ro : readOnly) {
        if (cmd == ro) return true;
    }
    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"EdgeMonitor_SingleInstance");
    if (!g_singleInstanceMutex) return true; // Can't guard; don't block.
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "Edge Monitor is already running. Close it or use --force to run another instance."
                  << std::endl;
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);

    std::vector<std::string> args(argv + 1, argv + argc);
    std::string cmd = args.empty() ? "gui" : args[0];

    if (!acquireSingleInstance(cmd)) {
        return 2;
    }

    g_engine = std::make_unique<MonitorEngine>();
    if (!g_engine->initialize()) {
        std::cerr << "Failed to initialize application" << std::endl;
        return 1;
    }

    if (cmd == "discover") {
        printDiscovery(g_engine->discover());
        return 0;
    }

    if (cmd == "targets") {
        printTargets(g_engine->targets());
        return 0;
    }

    if (cmd == "lock") {
        if (args.size() < 2) {
            std::cerr << "Usage: edge-monitor.exe lock <url> [ocr]" << std::endl;
            return 1;
        }
        bool ocr = args.size() > 2 && args[2] == "ocr";
        auto id = g_engine->lockTarget(args[1], ocr);
        if (id.empty()) {
            std::cerr << "Failed to lock target: " << args[1] << std::endl;
            return 1;
        }
        std::cout << "Locked target " << id.value << std::endl;
        return 0;
    }

    if (cmd == "unlock") {
        if (args.size() < 2) {
            std::cerr << "Usage: edge-monitor.exe unlock <target-id>" << std::endl;
            return 1;
        }
        g_engine->unlockTarget(args[1]);
        std::cout << "Unlocked target " << args[1] << std::endl;
        return 0;
    }

    if (cmd == "start") {
        g_engine->start();
        std::cout << "Monitor started. Press Ctrl+C to stop." << std::endl;
        while (!g_interrupted.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return 0;
    }

    if (cmd == "stop") {
        g_engine->stop();
        std::cout << "Monitor stopped." << std::endl;
        return 0;
    }

    if (cmd == "status") {
        printStatus(*g_engine);
        return 0;
    }

    if (cmd == "ocr") {
        if (args.size() < 2) {
            std::cerr << "Usage: edge-monitor.exe ocr <target-id>" << std::endl;
            return 1;
        }
        g_engine->runOCR(args[1]);
        return 0;
    }

    if (cmd == "search") {
        if (args.size() < 2) {
            std::cerr << "Usage: edge-monitor.exe search \"query\"" << std::endl;
            return 1;
        }
        auto results = g_engine->search(args[1], 10);
        std::cout << "\n=== Search Results ===" << std::endl;
        std::cout << "Query: " << args[1] << std::endl;
        std::cout << "Found: " << results.size() << std::endl;
        for (size_t i = 0; i < results.size(); ++i) {
            std::cout << "[" << (i + 1) << "] score=" << results[i].score
                      << "  " << results[i].content.substr(0, 100) << std::endl;
        }
        return 0;
    }

#ifdef _WIN32
    if (cmd == "gui") {
        // No g_engine->start(): the monitor loop's repeated discovery churn
        // destabilizes CDP websockets. The GUI launches LinkWatcher on demand.
        Logger::addSink([](const std::string& line) {
            live::pushLog(line);
        });
        guiThread(g_engine.get());
        return 0;
    }

    if (cmd == "watch") {
        if (args.size() < 2) {
            std::cerr << "Usage: edge-monitor.exe watch <url> [--select]" << std::endl;
            return 1;
        }
        std::string watchUrl = normalizeUrl(args[1]);
        bool select = std::find(args.begin(), args.end(), "--select") != args.end();
        Logger::addSink([](const std::string& line) {
            live::pushLog(line);
        });
        auto logCb = [](const std::string& line) { live::pushLog(line); };
        auto monitor = g_engine->startExactWatch(watchUrl, select, logCb);
        if (!monitor) {
            std::cerr << "Watch failed: no attachable exact target. "
                         "Check the CDP-enabled Edge is running and the page is open."
                      << std::endl;
            return 1;
        }
        std::cout << "Watching exact page " << watchUrl
                  << " -> reports/" << monitor->report()->targetId()
                  << "/. Ctrl+C to stop." << std::endl;
        std::cout << "Press Ctrl+C to stop." << std::endl;
        while (!g_interrupted.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        // Graceful shutdown: destroying the monitor stops its loop and writes
        // the final report via the report destructor.
        monitor->stop();
        return 0;
    }

    if (cmd == "watch-links") {
        if (args.size() < 2) {
            std::cerr << "Usage: edge-monitor.exe watch-links <url>" << std::endl;
            return 1;
        }
        Logger::addSink([](const std::string& line) {
            live::pushLog(line);
        });
        std::string watchUrl = normalizeUrl(args[1]);
        // Secondary mode: requires live CDP (LinkWatcher checks + reports the
        // exact state; it never relaunches Edge).
        LinkWatcher watcher(*g_engine, watchUrl, g_engine->pool());
        watcher.start();
        std::cout << "Watching " << watchUrl << " and all its links. "
                  << "Saving OCR results to save-result/. Press Ctrl+C to stop."
                  << std::endl;
        while (!g_interrupted.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        watcher.stop();
        return 0;
    }
#endif

    printHelp();
    return cmd == "help" ? 0 : 1;
}
