#pragma once

// trading/net/TlsInit.hpp — OpenSSL TLS helpers shared by HttpClient and
// WebSocketClient. Loads the Windows system certificate store into an
// SSL_CTX so TLS verification works out of the box (OpenSSL on Windows does
// NOT find system roots via set_default_verify_paths).

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <string>

namespace trading {

// Populates ctx's trust store from the Windows "ROOT" and "CA" system stores.
// Returns true when at least one CA was added. Safe to call per-connection.
bool loadWindowsRootCerts(SSL_CTX* ctx);

// Convenience: creates a client SSL_CTX with verification configured and the
// Windows roots loaded.
SSL_CTX* createVerifiedClientCtx(std::string& error);

} // namespace trading
