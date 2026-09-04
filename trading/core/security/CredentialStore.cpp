#include "trading/core/security/CredentialStore.hpp"
#include "logger.hpp"

#include <windows.h>
#include <wincred.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>

namespace trading {

// ---------------------------------------------------------------------------
// Windows Credential Manager helpers
// ---------------------------------------------------------------------------

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    return w;
}

std::string toNarrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring credTarget(const std::string& connector, const std::string& field) {
    std::string t = "tradingcore/" + connector + "/" + field;
    return toWide(t);
}

bool credManagerWrite(const std::string& connector, const std::string& field,
                      const std::string& value) {
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<wchar_t*>(credTarget(connector, field).c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(value.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) != FALSE;
}

std::optional<std::string> credManagerRead(const std::string& connector,
                                           const std::string& field) {
    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(credTarget(connector, field).c_str(), CRED_TYPE_GENERIC, 0, &pcred))
        return std::nullopt;
    std::string out(reinterpret_cast<char*>(pcred->CredentialBlob),
                    pcred->CredentialBlobSize);
    CredFree(pcred);
    return out;
}

bool credManagerDelete(const std::string& connector, const std::string& field) {
    return CredDeleteW(credTarget(connector, field).c_str(), CRED_TYPE_GENERIC, 0) != FALSE;
}

// DPAPI protect/unprotect. scope is local machine so the file survives the
// same user session but is bound to this Windows installation.
std::optional<std::vector<uint8_t>> dpapiProtect(const std::vector<uint8_t>& plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 const_cast<BYTE*>(plain.data())};
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, L"trading-core credentials", nullptr, nullptr,
                          nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::vector<uint8_t> res(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return res;
}

std::optional<std::vector<uint8_t>> dpapiUnprotect(const std::vector<uint8_t>& cipher) {
    DATA_BLOB in{static_cast<DWORD>(cipher.size()),
                 const_cast<BYTE*>(const_cast<uint8_t*>(cipher.data()))};
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::vector<uint8_t> res(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return res;
}

// ---------------------------------------------------------------------------
// Encrypted file format: one entry per line  "key<0x1F>b64cipher<0x1E>..."
// Header line "TCF1". Base64 via CryptBinaryToString.
// ---------------------------------------------------------------------------

std::string b64encode(const std::vector<uint8_t>& data) {
    DWORD needed = 0;
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed);
    std::string out(needed, '\0');
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &needed);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::vector<uint8_t> b64decode(const std::string& s) {
    DWORD needed = 0;
    CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
                         CRYPT_STRING_BASE64, nullptr, &needed, nullptr, nullptr);
    std::vector<uint8_t> out(needed);
    CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
                         CRYPT_STRING_BASE64, out.data(), &needed, nullptr, nullptr);
    out.resize(needed);
    return out;
}

std::map<std::string, std::string> readFileEntries(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    if (!in.is_open()) return out;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.size() < 3) continue;
        auto sep = line.find('\x1F');
        if (sep == std::string::npos) continue;
        std::string key = line.substr(0, sep);
        std::string b64 = line.substr(sep + 1);
        if (!b64.empty() && b64.back() == '\r') b64.pop_back();
        auto cipher = b64decode(b64);
        auto plain = dpapiUnprotect(cipher);
        if (plain) out[key] = std::string(plain->begin(), plain->end());
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// CredentialStore
// ---------------------------------------------------------------------------

void CredentialStore::setEncryptedFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    filePath_ = path;
}

bool CredentialStore::writeFileEntry(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (filePath_.empty()) return false;
    // Read existing entries (plaintext after DPAPI unprotect).
    auto entries = readFileEntries(filePath_);
    entries[key] = value;  // plaintext value
    return writeEntriesLocked(entries);
}

std::optional<std::string> CredentialStore::readFileEntry(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (filePath_.empty()) return std::nullopt;
    auto entries = readFileEntries(filePath_);
    auto it = entries.find(key);
    if (it == entries.end()) return std::nullopt;
    // entries already contains plaintext after DPAPI unprotect in readFileEntries
    return it->second;
}

bool CredentialStore::deleteFileEntry(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (filePath_.empty()) return false;
    auto entries = readFileEntries(filePath_);
    auto it = entries.find(key);
    if (it == entries.end()) return false;
    entries.erase(it);
    return writeEntriesLocked(entries);
}

// Serializes the entry map, DPAPI-encrypting every value. No plaintext ever
// reaches disk. Caller must hold mutex_.
bool CredentialStore::writeEntriesLocked(
    const std::map<std::string, std::string>& entries) {
    std::ofstream out(filePath_, std::ios::trunc);
    if (!out.is_open()) return false;
    out << "TCF1\n";
    for (const auto& [k, v] : entries) {
        std::vector<uint8_t> plain(v.begin(), v.end());
        auto cipher = dpapiProtect(plain);
        if (!cipher) {
            LOG_ERROR("CredentialStore: DPAPI protect failed");
            return false;
        }
        out << k << '\x1F' << b64encode(*cipher) << '\n';
    }
    return true;
}

CredentialSource CredentialStore::locate(const std::string& connector,
                                         const std::string& field,
                                         const std::string& envVar,
                                         std::string& out) const {
    // 1. Credential Manager
    if (auto v = credManagerRead(connector, field)) {
        out = *v;
        return CredentialSource::CredManager;
    }
    // 2. Encrypted file
    if (filePath_ != "") {
        std::lock_guard<std::mutex> lock(mutex_);
        auto entries = readFileEntries(filePath_);
        auto it = entries.find(connector + "/" + field);
        if (it != entries.end()) {
            out = it->second;
            return CredentialSource::EncryptedFile;
        }
    }
    // 3. Environment variable
    if (!envVar.empty()) {
        const char* v = std::getenv(envVar.c_str());
        if (v && *v) {
            out = v;
            return CredentialSource::Environment;
        }
    }
    return CredentialSource::None;
}

std::optional<std::string> CredentialStore::get(const std::string& connector,
                                                const std::string& field,
                                                const std::string& envVar) const {
    std::string out;
    auto src = locate(connector, field, envVar, out);
    if (src == CredentialSource::None) return std::nullopt;
    return out;
}

bool CredentialStore::set(const std::string& connector, const std::string& field,
                          const std::string& value, bool alsoCredManager) {
    if (value.empty()) return false;
    bool ok = true;
    if (alsoCredManager) {
        ok = credManagerWrite(connector, field, value);
    }
    // Always mirror into the encrypted file when a path is configured.
    if (filePath_ != "") {
        ok = writeFileEntry(connector + "/" + field, value) && ok;
    }
    return ok;
}

bool CredentialStore::remove(const std::string& connector, const std::string& field) {
    bool ok = false;
    if (filePath_ != "") ok = deleteFileEntry(connector + "/" + field) || ok;
    ok = credManagerDelete(connector, field) || ok;
    return ok;
}

std::string CredentialStore::mask(const std::string& secret) {
    if (secret.empty()) return "";
    constexpr size_t kTail = 4;
    constexpr size_t kMaxMasked = 12;
    size_t n = secret.size();
    if (n <= kTail + 1) return std::string(n, '*');
    size_t visible = (std::min)(n - kTail, kMaxMasked);
    std::string out = secret.substr(0, visible);
    out += std::string(n - visible - kTail, '*');
    out += secret.substr(n - kTail);
    return out;
}

std::string CredentialStore::redact(const std::string& text) {
    // Conservative generic redaction for common secret prefixes.
    // Full protection comes from never logging secrets in the first place.
    std::string out = text;
    static const char* kPatterns[] = {
        "sk_live_", "sk_test_", "pk_live_", "pk_test_", "AKIA",
        "api_key=", "apikey=", "secret=", "token=", "Bearer "
    };
    for (const char* pat : kPatterns) {
        const size_t patLen = std::char_traits<char>::length(pat);
        size_t pos = 0;
        while ((pos = out.find(pat, pos)) != std::string::npos) {
            // The secret value begins right after the pattern label.
            size_t valStart = pos + patLen;
            size_t end = valStart;
            while (end < out.size() && (std::isalnum(static_cast<unsigned char>(out[end])) ||
                                        out[end] == '-' || out[end] == '_' || out[end] == '.'))
                ++end;
            // Only mask when a real value follows (avoid mangling bare labels).
            if (end > valStart + 4) {
                out.replace(valStart, end - valStart, std::string(end - valStart, '*'));
            }
            pos = end;
        }
    }
    return out;
}

} // namespace trading
