// Unit tests for the credential store (Phase 3): masking, env fallback,
// DPAPI-encrypted file round-trip, redaction, and no-plaintext persistence.
#include "trading/core/security/CredentialStore.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace trading;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); ++g_fail; } \
    else { std::printf("[PASS] %s\n", msg); } \
} while (0)

int main() {
    // --- Masking ---
    CHECK(CredentialStore::mask("sk_live_abcdefghijklmnop") ==
              "sk_live_abcd********mnop",
          "mask keeps head + 4 tail chars");
    CHECK(CredentialStore::mask("short") == "*****", "short secrets fully masked");
    CHECK(CredentialStore::mask("") == "", "empty masks to empty");
    CHECK(CredentialStore::mask("x") == "*", "single char masks to one star");

    // Redaction hides known secret bodies
    std::string red = CredentialStore::redact("key=sk_live_ABC123DEF456 token=xyz");
    CHECK(red.find("ABC123DEF456") == std::string::npos,
          "redact removes sk_live_ secret body");
    CHECK(red.find("sk_live_") != std::string::npos,
          "redact keeps the prefix label");

    // --- Environment fallback ---
    _putenv_s("TC_TEST_ALPACA_KEY", "env-secret-value-123");
    CredentialStore store;
    auto fromEnv = store.get("alpaca", cred::API_KEY, "TC_TEST_ALPACA_KEY");
    CHECK(fromEnv.has_value() && *fromEnv == "env-secret-value-123",
          "reads secret from environment variable");

    // --- Encrypted file round-trip ---
    std::string path =
        (std::filesystem::temp_directory_path() / "trading_cred_test.bin").string();
    std::remove(path.c_str());
    CredentialStore fstore;
    fstore.setEncryptedFile(path);
    CHECK(fstore.set("alpaca", cred::API_SECRET, "super-secret-value", false),
          "set writes DPAPI-encrypted entry");
    auto back = fstore.get("alpaca", cred::API_SECRET);
    CHECK(back.has_value() && *back == "super-secret-value",
          "get decrypts DPAPI entry");

    // File must not contain plaintext secret
    {
        std::ifstream in(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        CHECK(content.find("super-secret-value") == std::string::npos,
              "secret never stored in plain text");
        CHECK(!content.empty() && content.find("TCF1") == 0,
              "file has encrypted header");
    }

    // get on a different store instance (same file) works — DPAPI is user-scoped
    CredentialStore fstore2;
    fstore2.setEncryptedFile(path);
    auto back2 = fstore2.get("alpaca", cred::API_SECRET);
    CHECK(back2.has_value() && *back2 == "super-secret-value",
          "second instance reads the same encrypted file");

    // Missing secret => nullopt, not error
    CHECK(!fstore.get("alpaca", cred::PASSPHRASE).has_value(),
          "missing secret returns nullopt");

    // Remove
    CHECK(fstore.remove("alpaca", cred::API_SECRET), "remove deletes entry");
    CHECK(!fstore.get("alpaca", cred::API_SECRET).has_value(),
          "secret gone after remove");

    std::remove(path.c_str());
    _putenv_s("TC_TEST_ALPACA_KEY", "");

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
