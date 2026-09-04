#include "edge/EdgeDiscovery.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    bool success = false;
};

#ifdef _WIN32
// Winsock must be initialized exactly once per process; CDPClient also uses
// Winsock, so never call WSACleanup here (that would tear down the process-wide
// socket state).
void ensureWinsock() {
    static bool init = [] {
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }();
    (void)init;
}

// Winsock-based HTTP GET with hard timeouts. Replaces WinINet entirely:
// WinINet's timeout behavior for closed ports and its read path proved
// unreliable in this app, while raw sockets give precise, observable control.
bool socketHttpGet(const std::string& host, int port, const std::string& path,
                   int timeoutMs, std::string& outBody) {
    ensureWinsock();

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

    // Send the HTTP request. The Host header MUST include the port: Edge builds
    // the returned webSocketDebuggerUrl from the Host header, so a bare
    // "127.0.0.1" yields a port-less (broken) websocket URL.
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
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

    // Read the response until connection close or timeout. The body can be
    // larger than one packet (the /json target list), so allow up to 4x the
    // base timeout for the read phase; the connect phase stays fast.
    outBody.clear();
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs * 4);
    while (true) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            outBody.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) break;  // peer closed
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 50 * 1000;
            fd_set rset;
            FD_ZERO(&rset);
            FD_SET(sock, &rset);
            select(0, &rset, nullptr, nullptr, &tv);
            continue;
        }
        break;  // real error
    }

    closesocket(sock);

    // Extract body after the header terminator \r\n\r\n.
    size_t pos = outBody.find("\r\n\r\n");
    if (pos != std::string::npos) {
        outBody = outBody.substr(pos + 4);
        return true;
    }
    return false;
}

HttpResponse httpGet(const std::string& url, int timeoutMs = 2000) {
    HttpResponse response;

    // Parse http://host:port/path
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return response;
    size_t hostStart = schemeEnd + 3;
    size_t colon = url.find(':', hostStart);
    size_t slash = url.find('/', hostStart);
    std::string host = url.substr(hostStart, (colon == std::string::npos ? slash : colon) - hostStart);
    std::string portStr = (colon == std::string::npos)
        ? "80"
        : url.substr(colon + 1, (slash == std::string::npos ? url.size() : slash) - colon - 1);
    std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);

    try {
        int port = std::stoi(portStr);
        std::string body;
        if (socketHttpGet(host, port, path, timeoutMs, body)) {
            response.body = body;
            response.statusCode = 200;
            response.success = true;
        }
    } catch (...) {
        // fall through
    }

    return response;
}

#endif  // _WIN32

}

namespace edgemon {

EdgeDiscovery::EdgeDiscovery(const DiscoveryConfig& config) : config_(config) {}

void EdgeDiscovery::setConfig(const DiscoveryConfig& config) {
    config_ = config;
}

std::vector<EdgeInstance> EdgeDiscovery::discoverAllInstances() {
    std::vector<EdgeInstance> instances;
    
    for (int port : config_.preferredPorts) {
        if (auto instance = probePort(port)) {
            instances.push_back(*instance);
        }
    }
    
    LOG_INFO("EdgeDiscovery: found {} instances", instances.size());
    return instances;
}

std::optional<EdgeInstance> EdgeDiscovery::discoverByProcessId(EdgeProcessId pid) {
    if (pid.empty()) return std::nullopt;
    
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    
    std::optional<EdgeInstance> result;
    
    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == pid.value) {
                EdgeInstance instance;
                instance.processId = pid;
                
                std::wstring exePath(pe.szExeFile);
                instance.executablePath.assign(exePath.begin(), exePath.end());
                
                result = instance;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }
    
    CloseHandle(snapshot);
    return result;
}

std::vector<EdgeInstance> EdgeDiscovery::discoverByPort(int port) {
    std::vector<EdgeInstance> instances;
    if (auto instance = probePort(port)) {
        instances.push_back(*instance);
    }
    return instances;
}

std::vector<EdgeTarget> EdgeDiscovery::enumerateTargets(const EdgeInstance& instance) {
    if (instance.debuggingPort == 0) {
        return {};
    }
    
    std::string url = "http://127.0.0.1:" + std::to_string(instance.debuggingPort) + "/json";
    auto response = httpGet(url, config_.timeoutMs);
    
    if (!response.success) {
        LOG_WARN("EdgeDiscovery: failed to enumerate targets on port {}", instance.debuggingPort);
        return {};
    }
    
    return parseTargetList(response.body);
}

std::vector<EdgeTarget> EdgeDiscovery::enumerateAllTargets() {
    std::vector<EdgeTarget> allTargets;
    
    auto instances = discoverAllInstances();
    for (const auto& instance : instances) {
        auto targets = enumerateTargets(instance);
        allTargets.insert(allTargets.end(), targets.begin(), targets.end());
    }
    
    LOG_INFO("EdgeDiscovery: enumerated {} total targets", allTargets.size());
    return allTargets;
}

std::optional<EdgeTarget> EdgeDiscovery::findTargetByUrl(const EdgeInstance& instance, const PageUrl& url) {
    auto targets = enumerateTargets(instance);
    for (const auto& target : targets) {
        if (target.url.contains(url.value)) {
            return target;
        }
    }
    return std::nullopt;
}

std::optional<EdgeTarget> EdgeDiscovery::findTargetByTitle(const EdgeInstance& instance, const std::string& title) {
    auto targets = enumerateTargets(instance);
    for (const auto& target : targets) {
        if (target.title.find(title) != std::string::npos) {
            return target;
        }
    }
    return std::nullopt;
}

std::optional<EdgeTarget> EdgeDiscovery::findTargetById(const EdgeInstance& instance, const CDPTargetId& id) {
    auto targets = enumerateTargets(instance);
    for (const auto& target : targets) {
        if (target.id == id) {
            return target;
        }
    }
    return std::nullopt;
}

bool EdgeDiscovery::isEdgeRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    
    bool found = false;
    if (Process32FirstW(snapshot, &pe)) {
        do {
            std::wstring exeName(pe.szExeFile);
            if (exeName == L"msedge.exe") {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }
    
    CloseHandle(snapshot);
    return found;
}

std::string EdgeDiscovery::buildWebSocketUrl(const EdgeInstance& instance, const CDPTargetId& targetId) {
    return "ws://localhost:" + std::to_string(instance.debuggingPort) + 
           "/devtools/page/" + targetId.value;
}

EdgeDiscovery::DiscoveryResult EdgeDiscovery::discover() {
    DiscoveryResult result;
    
    try {
        auto instances = discoverAllInstances();
        result.instances = instances;
        
        for (const auto& instance : instances) {
            auto targets = enumerateTargets(instance);
            result.targets.insert(result.targets.end(), targets.begin(), targets.end());
        }
        
        result.success = !result.instances.empty();
        
        if (!result.success) {
            result.errorMessage = "No Edge instances found with debugging enabled";
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("Discovery failed: ") + e.what();
        LOG_ERROR("EdgeDiscovery: {}", result.errorMessage);
    }
    
    return result;
}

std::optional<EdgeInstance> EdgeDiscovery::probePort(int port) {
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/json";
    
    if (!probeEndpoint(url)) {
        return std::nullopt;
    }
    
    EdgeInstance instance;
    instance.debuggingPort = port;
    
    auto response = httpGet(url, config_.timeoutMs);
    if (response.success && !response.body.empty()) {
        instance.browserVersion = "Edge";
    }
    
    LOG_DEBUG("EdgeDiscovery: found Edge on port {}", port);
    return instance;
}

bool EdgeDiscovery::probeEndpoint(const std::string& url) {
    auto response = httpGet(url, config_.timeoutMs);
    return response.success;
}

std::vector<EdgeTarget> EdgeDiscovery::parseTargetList(const std::string& jsonResponse) {
    std::vector<EdgeTarget> targets;

    if (jsonResponse.empty()) {
        return targets;
    }

    try {
        auto parsed = nlohmann::json::parse(jsonResponse);
        if (!parsed.is_array()) {
            LOG_WARN("EdgeDiscovery: /json response is not an array");
            return targets;
        }

        for (const auto& entry : parsed) {
            if (!entry.is_object()) continue;

            EdgeTarget target;
            target.id = CDPTargetId(entry.value("id", std::string()));
            target.type = entry.value("type", std::string());
            target.title = entry.value("title", std::string());
            target.url = PageUrl(entry.value("url", std::string()));
            target.webSocketDebuggerUrl = entry.value("webSocketDebuggerUrl", std::string());
            if (entry.contains("pid") && entry["pid"].is_number()) {
                target.processId = EdgeProcessId(entry["pid"].get<uint32_t>());
            }

            if (!target.id.empty() && (target.isPage() || target.isBackgroundPage())) {
                targets.push_back(target);
                LOG_DEBUG("EdgeDiscovery: found target {} - {} ({})",
                          target.id.value, target.title, target.url.value);
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("EdgeDiscovery: failed to parse target list: {}", e.what());
    }

    return targets;
}

} // namespace edgemon
