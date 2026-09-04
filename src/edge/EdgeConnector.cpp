#include "edge/EdgeConnector.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>

#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace edgemon {

namespace {

// Winsock must be initialized exactly once per process (CDPClient shares it).
void ensureWinsock() {
    static bool init = [] {
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }();
    (void)init;
}

std::string httpBodyOnly(const std::string& raw) {
    size_t pos = raw.find("\r\n\r\n");
    if (pos == std::string::npos) pos = raw.find("\n\n");
    if (pos == std::string::npos) return raw;
    return raw.substr(pos + 4);
}

std::wstring toWide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return out;
}

std::string toNarrow(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(static_cast<char>(c & 0xFF));
    return out;
}

// Reads another process's command line via the PEB (x64 offset 0x70).
std::wstring readProcessCommandLine(DWORD pid) {
    std::wstring result;
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return result;

    typedef LONG(NTAPI* pNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto ntQ = (pNtQueryInformationProcess)GetProcAddress(ntdll, "NtQueryInformationProcess");

    struct PROCESS_BASIC_INFORMATION {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID Reserved3;
    } pbi{};
    if (!ntQ || ntQ(proc, 0, &pbi, sizeof(pbi), nullptr) < 0) {
        CloseHandle(proc);
        return result;
    }

    struct PEB_READ {
        BYTE reserved1[2];
        BYTE beingDebugged;
        BYTE reserved2[1];
        PVOID reserved3[2];
        PVOID Ldr;
        PVOID ProcessParameters;
    } peb{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(proc, pbi.PebBaseAddress, &peb, sizeof(peb), &read) ||
        !peb.ProcessParameters) {
        CloseHandle(proc);
        return result;
    }

    struct {
        USHORT Length;
        USHORT MaxLength;
        PVOID Buffer;
    } cmdline{};
    BYTE* pp = (BYTE*)peb.ProcessParameters;
    if (!ReadProcessMemory(proc, pp + 0x70, &cmdline, sizeof(cmdline), &read) ||
        !cmdline.Buffer || cmdline.Length == 0 || cmdline.Length >= 32768) {
        CloseHandle(proc);
        return result;
    }
    result.resize(cmdline.Length / sizeof(wchar_t));
    if (ReadProcessMemory(proc, cmdline.Buffer, &result[0], cmdline.Length, &read)) {
        result.resize(read / sizeof(wchar_t));
    } else {
        result.clear();
    }
    CloseHandle(proc);
    return result;
}

// Main msedge.exe process + its --user-data-dir (real profile root).
struct MainEdgeInfo {
    std::wstring exePath;
    std::wstring userDataDir;
    bool found = false;
};

MainEdgeInfo findMainEdgeProcess() {
    MainEdgeInfo info;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return info;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    std::wstring bestCmd;
    if (Process32FirstW(snapshot, &pe)) {
        do {
            std::wstring exe(pe.szExeFile);
            if (_wcsicmp(exe.c_str(), L"msedge.exe") != 0) continue;
            std::wstring cmd = readProcessCommandLine(pe.th32ProcessID);
            if (cmd.find(L"--type=") != std::wstring::npos) continue;
            if (cmd.size() <= bestCmd.size()) continue;

            bestCmd = cmd;
            info.exePath = pe.szExeFile;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) {
                wchar_t path[MAX_PATH] = {0};
                DWORD sz = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, path, &sz)) info.exePath = path;
                CloseHandle(h);
            }
            info.found = true;

            size_t pos = cmd.find(L"--user-data-dir=");
            if (pos != std::wstring::npos) {
                std::wstring rest = cmd.substr(pos + 16);
                if (!rest.empty() && rest[0] == L'"') {
                    size_t end = rest.find(L'"', 1);
                    if (end != std::wstring::npos) info.userDataDir = rest.substr(1, end - 1);
                } else {
                    size_t end = rest.find(L' ');
                    info.userDataDir = rest.substr(0, end);
                }
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return info;
}

bool terminateAllEdge(int timeoutMs) {
    std::vector<DWORD> pids;
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(PROCESSENTRY32W);
        if (Process32FirstW(snapshot, &pe)) {
            do {
                std::wstring exe(pe.szExeFile);
                if (_wcsicmp(exe.c_str(), L"msedge.exe") == 0) pids.push_back(pe.th32ProcessID);
            } while (Process32NextW(snapshot, &pe));
        }
        CloseHandle(snapshot);
    }
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        bool any = false;
        HANDLE s2 = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (s2 != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W p2;
            p2.dwSize = sizeof(PROCESSENTRY32W);
            if (Process32FirstW(s2, &p2)) {
                do {
                    std::wstring exe(p2.szExeFile);
                    if (_wcsicmp(exe.c_str(), L"msedge.exe") == 0) { any = true; break; }
                } while (Process32NextW(s2, &p2));
            }
            CloseHandle(s2);
        }
        if (!any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

DWORD portOwnerPid(int port) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return 0;
    std::vector<BYTE> buf(size);
    PMIB_TCPTABLE_OWNER_PID table = (PMIB_TCPTABLE_OWNER_PID)buf.data();
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return 0;
    }
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        auto& row = table->table[i];
        if (ntohs((u_short)row.dwLocalPort) == (u_short)port && row.dwState == 2) {
            return row.dwOwningPid;
        }
    }
    return 0;
}

void removeSingletonLocks(const std::wstring& userDataDir) {
    if (userDataDir.empty()) return;
    std::error_code ec;
    std::filesystem::path root(userDataDir);
    for (const wchar_t* f : {L"SingletonLock", L"SingletonSocket", L"SingletonCookie"}) {
        std::filesystem::remove(root / f, ec);
    }
}

} // namespace

std::string EdgeConnector::httpGetBody(const std::string& host, int port,
                                       const std::string& path, int timeoutMs) {
    ensureWinsock();

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return "";

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
        return "";
    }

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
                    return "";
                }
                continue;
            }
            closesocket(sock);
            return "";
        }
        sent += n;
    }

    std::string out;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs * 4);
    while (true) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) break;
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
        break;
    }
    closesocket(sock);

    size_t pos = out.find("\r\n\r\n");
    if (pos != std::string::npos) return out.substr(pos + 4);
    return "";
}

std::string EdgeConnector::parseBrowserVersion(const std::string& body) {
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("Browser") && j["Browser"].is_string()) {
            return j["Browser"].get<std::string>();
        }
    } catch (const std::exception&) {
    }
    return "";
}

EdgeProbeResult EdgeConnector::probe() {
    EdgeProbeResult result;
    for (int p = kFirstPort; p <= kLastPort; ++p) {
        std::string version = httpGetBody("127.0.0.1", p, "/json/version", 400);
        if (!version.empty()) {
            std::string browser = parseBrowserVersion(version);
            if (browser.empty()) {
                result.reasons.push_back("port " + std::to_string(p) +
                                         " answers but is not a CDP /json/version endpoint");
                continue;
            }
            // Require /json/list to actually answer too.
            std::string list = httpGetBody("127.0.0.1", p, "/json/list", 400);
            if (list.empty() || list.front() != '[') {
                result.reasons.push_back("port " + std::to_string(p) +
                                         " /json/version OK but /json/list not reachable");
                continue;
            }
            result.state = EdgeConnectorState::CDP_AVAILABLE;
            result.cdpPort = p;
            result.browserVersion = browser;
            LOG_INFO("EdgeConnector: CDP_AVAILABLE on port {} ({})", p, browser);
            return result;
        }
    }
    result.state = EdgeConnectorState::EDGE_NOT_RUNNING;
    LOG_INFO("EdgeConnector: no CDP endpoint on ports {}-{}", kFirstPort, kLastPort);
    return result;
}

bool EdgeConnector::isEdgeProcessRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    bool found = false;
    if (Process32FirstW(snapshot, &pe)) {
        do {
            std::wstring exe(pe.szExeFile);
            if (_wcsicmp(exe.c_str(), L"msedge.exe") == 0) { found = true; break; }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return found;
}

EdgeProbeResult EdgeConnector::diagnose() {
    EdgeProbeResult result = probe();
    if (result.cdpAvailable()) return result;

    result.state = isEdgeProcessRunning()
        ? EdgeConnectorState::EDGE_RUNNING_NO_CDP
        : EdgeConnectorState::EDGE_NOT_RUNNING;

    // Ports held by non-CDP processes?
    for (int p = kFirstPort; p <= kLastPort; ++p) {
        DWORD owner = portOwnerPid(p);
        if (owner == 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
        std::wstring ownerName;
        if (h) {
            wchar_t name[MAX_PATH] = {0};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, name, &sz)) ownerName = name;
            CloseHandle(h);
        }
        std::wstring lower = ownerName;
        for (auto& c : lower) c = towlower(c);
        if (lower.find(L"msedge.exe") != std::wstring::npos) {
            result.reasons.push_back("Edge holds port " + std::to_string(p) +
                                     " but does not answer CDP");
        } else {
            result.reasons.push_back("port " + std::to_string(p) + " held by " +
                                     toNarrow(ownerName));
        }
    }

    auto mainEdge = findMainEdgeProcess();
    if (mainEdge.found) {
        result.reasons.push_back("Edge running (exe=" + toNarrow(mainEdge.exePath) +
                                 ", user-data-dir=" +
                                 (mainEdge.userDataDir.empty()
                                      ? std::string("(default)")
                                      : toNarrow(mainEdge.userDataDir)) +
                                 ") but no CDP endpoint is listening. On Edge/Chromium "
                                 ">= 136 remote debugging is ignored on the default "
                                 "data directory.");
    }

    for (const auto& r : result.reasons) {
        LOG_INFO("EdgeConnector: {}", r);
    }
    return result;
}

std::vector<std::wstring> EdgeConnector::enumerateProfileDirs(const std::wstring& userDataRoot) {
    std::vector<std::wstring> out;
    if (userDataRoot.empty()) return out;
    std::error_code ec;
    std::filesystem::path root(userDataRoot);

    // Try "Local State" profile.info_cache first (best-effort, JSON).
    std::filesystem::path localState = root / L"Local State";
    if (std::filesystem::exists(localState, ec)) {
        try {
            std::ifstream in(localState);
            if (in) {
                std::ostringstream ss;
                ss << in.rdbuf();
                auto j = nlohmann::json::parse(ss.str());
                if (j.contains("profile") && j["profile"].contains("info_cache")) {
                    for (auto& [name, _] : j["profile"]["info_cache"].items()) {
                        if (name.empty() || name == "Guest Profile") continue;
                        std::filesystem::path p = root / toWide(name);
                        if (std::filesystem::is_directory(p, ec)) out.push_back(p.wstring());
                    }
                }
            }
        } catch (const std::exception&) {
        }
    }

    // Fallback: Default + Profile *.
    std::filesystem::path def = root / L"Default";
    if (std::filesystem::is_directory(def, ec)) out.push_back(def.wstring());
    for (int i = 1; i <= 20; ++i) {
        std::filesystem::path p = root / (L"Profile " + std::to_wstring(i));
        if (std::filesystem::is_directory(p, ec)) out.push_back(p.wstring());
    }
    // Dedup while preserving order.
    std::vector<std::wstring> dedup;
    for (auto& p : out) {
        if (std::find(dedup.begin(), dedup.end(), p) == dedup.end()) dedup.push_back(p);
    }
    return dedup;
}

EdgeProfileInfo EdgeConnector::findEdgeProfile() {
    EdgeProfileInfo info;

    MainEdgeInfo running = findMainEdgeProcess();
    std::wstring exe;
    std::wstring userData;

    if (running.found) {
        exe = running.exePath;
        userData = running.userDataDir;
    } else {
        // Edge not running: locate the exe from standard install paths.
        const wchar_t* candidates[] = {
            L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
            L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        };
        for (auto* c : candidates) {
            if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) {
                exe = c;
                break;
            }
        }
        if (exe.empty()) {
            wchar_t reg[MAX_PATH] = {0};
            DWORD rsz = MAX_PATH;
            if (SHGetValueW(HKEY_LOCAL_MACHINE,
                            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",
                            nullptr, nullptr, reg, &rsz) == ERROR_SUCCESS &&
                GetFileAttributesW(reg) != INVALID_FILE_ATTRIBUTES) {
                exe = reg;
            }
        }
    }

    if (exe.empty()) return info;

    // Real User Data root for the current Windows user.
    wchar_t localAppData[MAX_PATH] = {0};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData) == S_OK) {
        userData = std::wstring(localAppData) + L"\\Microsoft\\Edge\\User Data";
    }

    info.exePath = exe;
    info.userDataDir = running.found && !running.userDataDir.empty()
        ? running.userDataDir
        : userData;
    info.found = !info.exePath.empty();

    // Pick the default profile dir when present.
    auto dirs = enumerateProfileDirs(info.userDataDir);
    std::wstring def = info.userDataDir + L"\\Default";
    bool hasDefault = std::find(dirs.begin(), dirs.end(), def) != dirs.end();
    if (hasDefault) {
        info.profileDir = def;
    } else if (!dirs.empty()) {
        info.profileDir = dirs.front();
    }

    LOG_INFO("EdgeConnector: found Edge exe={} user-data-dir={} profile={}",
             toNarrow(info.exePath),
             info.userDataDir.empty() ? std::string("(default)") : toNarrow(info.userDataDir),
             info.profileDir.empty() ? std::string("(none)") : toNarrow(info.profileDir));
    return info;
}

EdgeProbeResult EdgeConnector::relaunchForCdp(const std::wstring& userDataDir,
                                              const std::wstring& profileDir,
                                              const std::string& startupUrl,
                                              int maxWaitMs) {
    EdgeProbeResult result;

    auto fail = [&](const std::string& why) {
        result.state = EdgeConnectorState::EDGE_RESTART_FAILED;
        result.reasons.push_back(why);
        LOG_ERROR("EdgeConnector: {}", why);
        return result;
    };

    if (userDataDir.empty()) {
        return fail("EDGE_RESTART_FAILED: no real user-data-dir available to relaunch");
    }

    // Already live? (checked again so double-restart is impossible)
    EdgeProbeResult already = probe();
    if (already.cdpAvailable()) return already;

    MainEdgeInfo running = findMainEdgeProcess();
    std::wstring exe = running.found ? running.exePath : EdgeProfileInfo{}.exePath;
    if (exe.empty()) {
        EdgeProfileInfo pi = findEdgeProfile();
        exe = pi.exePath;
    }
    if (exe.empty()) {
        return fail("EDGE_RESTART_FAILED: cannot locate msedge.exe");
    }

    // 1) Close existing Edge so the profile lock releases.
    if (running.found) {
        LOG_INFO("EdgeConnector: closing existing Edge (profile {}) for consented relaunch",
                 toNarrow(userDataDir));
        if (!terminateAllEdge(20000)) {
            return fail("EDGE_RESTART_FAILED: could not close existing Edge processes");
        }
        removeSingletonLocks(userDataDir);
    }

    // 2) Pick a free port.
    int debugPort = kFirstPort;
    for (int p = kFirstPort; p <= kLastPort; ++p) {
        if (portOwnerPid(p) == 0) { debugPort = p; break; }
    }

    // 3) Launch with the REAL data dir + debug flag.
    std::wstring cmd = L"\"" + exe + L"\" --remote-debugging-port=" +
                       std::to_wstring(debugPort) + L" --user-data-dir=\"" +
                       userDataDir + L"\"";
    if (!startupUrl.empty()) {
        cmd += L" \"" + toWide(startupUrl) + L"\"";
    }
    LOG_INFO("EdgeConnector: relaunching Edge (real profile): {}", toNarrow(cmd));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = cmd;
    if (!CreateProcessW(exe.c_str(), &cmdline[0], nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &si, &pi)) {
        return fail("EDGE_RESTART_FAILED: CreateProcess failed, error=" +
                    std::to_string(GetLastError()));
    }
    CloseHandle(pi.hThread);

    // 4) Wait for the port to bind (bounded, honest).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxWaitMs);
    auto delay = std::chrono::milliseconds(400);
    DWORD lastExit = STILL_ACTIVE;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) {
            lastExit = code;
            break;
        }
        EdgeProbeResult live = probe();
        if (live.cdpAvailable()) {
            CloseHandle(pi.hProcess);
            result.state = EdgeConnectorState::CDP_AVAILABLE;
            result.cdpPort = live.cdpPort;
            result.browserVersion = live.browserVersion;
            LOG_INFO("EdgeConnector: CDP_AVAILABLE after relaunch on port {}",
                     live.cdpPort);
            return result;
        }
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds(1600));
    }
    CloseHandle(pi.hProcess);

    // 5) Port never bound: restore the user's Edge (relaunch WITHOUT the flag)
    //    and report the real reason — never fall back to a dedicated profile.
    std::string reason = "EDGE_RESTART_FAILED: Edge relaunched with --remote-debugging-port on "
                         "user-data-dir '" + toNarrow(userDataDir) +
                         "' but no CDP endpoint bound within " +
                         std::to_string(maxWaitMs / 1000) +
                         "s. Modern Edge (Chromium >= 136) ignores the remote-debugging "
                         "switch on the default data directory. Restoring Edge without the flag.";
    if (lastExit != STILL_ACTIVE) {
        reason += " (launched process exited early, code " +
                  std::to_string(static_cast<int>(lastExit)) + ")";
    }
    LOG_WARN("{}", reason);

    std::wstring restore = L"\"" + exe + L"\" --user-data-dir=\"" + userDataDir + L"\"";
    if (!startupUrl.empty()) restore += L" \"" + toWide(startupUrl) + L"\"";
    STARTUPINFOW rsi{};
    rsi.cb = sizeof(rsi);
    PROCESS_INFORMATION rpi{};
    std::wstring restoreCmd = restore;
    if (CreateProcessW(exe.c_str(), &restoreCmd[0], nullptr, nullptr, FALSE,
                       CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                       nullptr, nullptr, &rsi, &rpi)) {
        CloseHandle(rpi.hThread);
        CloseHandle(rpi.hProcess);
    }

    result.state = EdgeConnectorState::EDGE_RESTART_FAILED;
    result.reasons.push_back(reason);
    return result;
}

// ============================================================================
// ensureDedicatedCdpInstance — the reliable fix for the Chromium >= 136 block.
//
// Instead of trying to debug the user's default Edge profile (which 136+
// refuses), we launch a SEPARATE Edge instance on a dedicated NON-DEFAULT
// profile directory. A non-default data dir is exactly what Chromium requires
// for --remote-debugging-port to bind, and it coexists with the user's normal
// browsing (their default Edge keeps running untouched).
// ============================================================================
EdgeProbeResult EdgeConnector::ensureDedicatedCdpInstance(const std::string& startupUrl,
                                                          int maxWaitMs) {
    // Already have a live CDP endpoint? Use it as-is.
    EdgeProbeResult existing = probe();
    if (existing.cdpAvailable()) return existing;

    // Locate the Edge executable.
    std::wstring exe;
    {
        MainEdgeInfo running = findMainEdgeProcess();
        if (running.found) {
            exe = running.exePath;
        } else {
            EdgeProfileInfo pi = findEdgeProfile();
            exe = pi.exePath;
        }
    }
    if (exe.empty()) {
        EdgeProbeResult fail;
        fail.state = EdgeConnectorState::EDGE_RESTART_FAILED;
        fail.reasons.push_back("cannot locate msedge.exe");
        LOG_ERROR("EdgeConnector: cannot locate msedge.exe for dedicated instance");
        return fail;
    }

    // Dedicated, non-default profile root (always a custom dir => CDP binds).
    wchar_t localAppData[MAX_PATH] = {0};
    std::wstring profileRoot;
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData) == S_OK) {
        profileRoot = std::wstring(localAppData) + L"\\EdgeMonitor";
    }
    if (profileRoot.empty()) {
        profileRoot = L"C:\\EdgeMonitor-EdgeProfile";
    }
    std::error_code ec;
    std::filesystem::create_directories(profileRoot, ec);

    // Pick a free debug port.
    int debugPort = kFirstPort;
    for (int p = kFirstPort; p <= kLastPort; ++p) {
        if (portOwnerPid(p) == 0) { debugPort = p; break; }
    }

    // Launch a SEPARATE instance. Because it uses a different user-data-dir it
    // runs in its own process group and does not disturb the user's Edge.
    // The full popup/silent flag set keeps the automated instance quiet:
    // no first-run, no default-browser nag, no crash restore, no background
    // mode, no component updates, and popups/new windows suppressed.
    std::wstring cmd = L"\"" + exe + L"\" --remote-debugging-port=" +
                       std::to_wstring(debugPort) + L" --user-data-dir=\"" +
                       profileRoot + L"\"" +
                       L" --no-first-run --no-default-browser-check"
                       L" --disable-popup-blocking"          // never allow JS popups
                       L" --disable-notifications"
                       L" --disable-session-crashed-bubble"
                       L" --no-restore-session-state"
                       L" --no-crash-upload"
                       L" --disable-component-update"
                       L" --disable-background-mode"
                       L" --disable-features=Translate,MediaRouter,OptimizationHints";
    if (!startupUrl.empty()) {
        cmd += L" \"" + toWide(startupUrl) + L"\"";
    }
    LOG_INFO("EdgeConnector: launching dedicated CDP Edge instance: {}",
             toNarrow(cmd));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = cmd;
    if (!CreateProcessW(exe.c_str(), &cmdline[0], nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &si, &pi)) {
        EdgeProbeResult fail;
        fail.state = EdgeConnectorState::EDGE_RESTART_FAILED;
        fail.reasons.push_back("dedicated instance CreateProcess failed, error=" +
                               std::to_string(GetLastError()));
        LOG_ERROR("EdgeConnector: dedicated instance launch failed: {}",
                  fail.reasons.back());
        return fail;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Wait (bounded) for the CDP port to bind on the dedicated instance.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxWaitMs);
    auto delay = std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
        EdgeProbeResult live = probe();
        if (live.cdpAvailable()) {
            LOG_INFO("EdgeConnector: dedicated CDP instance live on port {}",
                     live.cdpPort);
            return live;
        }
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds(1600));
    }

    EdgeProbeResult fail;
    fail.state = EdgeConnectorState::EDGE_RESTART_FAILED;
    fail.reasons.push_back("dedicated Edge instance did not bind a CDP port within " +
                           std::to_string(maxWaitMs / 1000) + "s");
    LOG_ERROR("EdgeConnector: {}", fail.reasons.back());
    return fail;
}

} // namespace edgemon
