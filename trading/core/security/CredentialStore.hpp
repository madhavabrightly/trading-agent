#pragma once

// trading/core/security/CredentialStore.hpp — secure secret storage.
//
// Backends (tried in priority order for reads, written to the strongest
// available):
//   1. Windows Credential Manager  (CredWriteW / CredReadW)
//   2. DPAPI-encrypted local file  (CryptProtectData / CryptUnprotectData)
//   3. Environment variables
//
// Secrets are never logged. The mask() helper redacts values for UI/log use.

#include <string>
#include <map>
#include <optional>
#include <mutex>

namespace trading {

// Known credential field names per connector. Connectors may read any of these;
// which fields apply depends on the broker.
namespace cred {
constexpr const char* API_KEY      = "api_key";
constexpr const char* API_SECRET   = "api_secret";
constexpr const char* ACCESS_TOKEN = "access_token";
constexpr const char* CLIENT_ID    = "client_id";
constexpr const char* ACCOUNT_ID   = "account_id";
constexpr const char* PASSPHRASE   = "passphrase";
constexpr const char* ENVIRONMENT  = "environment";
constexpr const char* REGION       = "region";
constexpr const char* ENDPOINT     = "endpoint";
} // namespace cred

enum class CredentialSource {
    None,        // not present anywhere
    Environment, // read from process environment
    CredManager, // Windows Credential Manager
    EncryptedFile // DPAPI-protected local file
};

class CredentialStore {
public:
    CredentialStore() = default;
    ~CredentialStore() = default;

    // Enables the DPAPI-encrypted local-file backend at the given path.
    void setEncryptedFile(const std::string& path);

    // Reads a secret. Looks in Credential Manager first (target
    // "tradingcore/<connector>/<field>"), then the encrypted file, then the
    // environment variable named envVar (when non-empty).
    std::optional<std::string> get(const std::string& connector,
                                   const std::string& field,
                                   const std::string& envVar = "") const;

    // Writes a secret to the encrypted file (and optionally Credential
    // Manager). Never writes secrets to plain files.
    bool set(const std::string& connector, const std::string& field,
             const std::string& value, bool alsoCredManager = false);

    bool remove(const std::string& connector, const std::string& field);

    // Redaction helpers (UI / logs).
    static std::string mask(const std::string& secret);
    // Scans text and replaces anything that looks like a masked secret tail —
    // for redacting log lines that may accidentally carry secrets.
    static std::string redact(const std::string& text);

private:
    // Returns the source actually backing a secret (None when absent).
    CredentialSource locate(const std::string& connector,
                            const std::string& field,
                            const std::string& envVar,
                            std::string& out) const;

    bool writeFileEntry(const std::string& key, const std::string& value);
    std::optional<std::string> readFileEntry(const std::string& key) const;
    bool deleteFileEntry(const std::string& key);

    // Serializes the entry map, DPAPI-encrypting every value. Caller holds
    // mutex_. Returns false on any encryption or I/O failure.
    bool writeEntriesLocked(const std::map<std::string, std::string>& entries);

    std::string filePath_;
    mutable std::mutex mutex_;
};

} // namespace trading
