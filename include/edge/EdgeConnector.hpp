#pragma once

// EdgeConnector — deterministic Edge/CDP availability state machine.
//
// The app must NEVER fabricate a dedicated profile or report "connected"
// unless /json/version and /json/list are actually reachable. Modern Edge
// (Chromium >= 136) silently drops --remote-debugging-port when launched
// against the default data directory, so an authenticated session can only be
// monitored when the browser is ALREADY running under CDP on a custom
// user-data-dir, or after a consented restart whose port bind is empirically
// verified. This class owns that decision.

#include "core/Types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace edgemon {

enum class EdgeConnectorState {
    CDP_AVAILABLE,               // /json/version + /json/list reachable
    EDGE_RUNNING_NO_CDP,         // msedge.exe running, no CDP endpoint
    EDGE_NOT_RUNNING,            // no msedge.exe and no CDP endpoint
    EDGE_RESTART_REQUIRED,       // consented restart pending (probe in flight)
    EDGE_RESTART_FAILED,         // restart attempted, port never bound
    TARGET_FOUND,                // resolution succeeded (convenience mirror)
    TARGET_NOT_FOUND,            // resolution failed deterministically
    AUTHENTICATED_SESSION_UNAVAILABLE  // no attachable authenticated session
};

constexpr const char* EdgeConnectorStateToString(EdgeConnectorState s) {
    switch (s) {
        case EdgeConnectorState::CDP_AVAILABLE:  return "CDP_AVAILABLE";
        case EdgeConnectorState::EDGE_RUNNING_NO_CDP: return "EDGE_RUNNING_NO_CDP";
        case EdgeConnectorState::EDGE_NOT_RUNNING: return "EDGE_NOT_RUNNING";
        case EdgeConnectorState::EDGE_RESTART_REQUIRED: return "EDGE_RESTART_REQUIRED";
        case EdgeConnectorState::EDGE_RESTART_FAILED: return "EDGE_RESTART_FAILED";
        case EdgeConnectorState::TARGET_FOUND:    return "TARGET_FOUND";
        case EdgeConnectorState::TARGET_NOT_FOUND: return "TARGET_NOT_FOUND";
        case EdgeConnectorState::AUTHENTICATED_SESSION_UNAVAILABLE:
            return "AUTHENTICATED_SESSION_UNAVAILABLE";
        default: return "UNKNOWN";
    }
}

struct EdgeProbeResult {
    EdgeConnectorState state = EdgeConnectorState::EDGE_NOT_RUNNING;
    int cdpPort = 0;              // when CDP_AVAILABLE
    std::string browserVersion;   // from /json/version "Browser"
    std::vector<std::string> reasons;  // exact reasons CDP is unavailable

    bool cdpAvailable() const { return state == EdgeConnectorState::CDP_AVAILABLE; }
};

struct EdgeProfileInfo {
    std::wstring exePath;
    std::wstring userDataDir;   // the User Data root (may be empty = default)
    std::wstring profileDir;    // resolved "Default"/"Profile N" under userDataDir
    bool found = false;
};

class EdgeConnector {
public:
    // Default ports probed for an already-live CDP endpoint.
    static constexpr int kFirstPort = 9222;
    static constexpr int kLastPort = 9230;

    // Probe standard ports; require BOTH /json/version and /json/list to
    // answer. Never reports CDP_AVAILABLE on a partial response.
    static EdgeProbeResult probe();

    // Process-level "is Edge running at all" (independent of CDP).
    static bool isEdgeProcessRunning();

    // Detailed availability diagnosis when CDP is absent.
    static EdgeProbeResult diagnose();

    // Locates the running (or default-install) Edge executable + real profile
    // dirs. Does NOT fabricate a directory.
    static EdgeProfileInfo findEdgeProfile();

    // Enumerate real profile directories under a User Data root, from
    // "Local State" profile.info_cache when parseable, else Default + Profile *.
    static std::vector<std::wstring> enumerateProfileDirs(const std::wstring& userDataRoot);

    // Consented real-profile restart: terminate Edge, relaunch the SAME real
    // user-data-dir with --remote-debugging-port, and verify the port binds.
    // When the port does not bind (default-dir CDP block), relaunch Edge again
    // WITHOUT the flag to restore the user's browsing, then report failure.
    // Never creates a dedicated/throwaway profile.
    static EdgeProbeResult relaunchForCdp(const std::wstring& userDataDir,
                                          const std::wstring& profileDir,
                                          const std::string& startupUrl,
                                          int maxWaitMs = 30000);

    // The pro fix for Chromium >= 136: launches a SEPARATE dedicated Edge
    // instance on a NON-DEFAULT profile dir (under %LOCALAPPDATA%/EdgeMonitor),
    // started with --remote-debugging-port so CDP genuinely binds. Coexists
    // with the user's normal/default Edge (nothing is killed or restarted).
    // If a CDP endpoint is already reachable it is returned unchanged.
    // startupUrl is opened in a new tab of the dedicated instance.
    static EdgeProbeResult ensureDedicatedCdpInstance(const std::string& startupUrl = "",
                                                      int maxWaitMs = 30000);

    // Minimal HTTP GET (raw socket) shared by probe + diagnose. Exposed for
    // TargetSelection / tests. Returns the raw response or "".
    static std::string httpGetBody(const std::string& host, int port,
                                   const std::string& path, int timeoutMs);

    // Parses the "Browser" field from a /json/version body.
    static std::string parseBrowserVersion(const std::string& body);
};

} // namespace edgemon
