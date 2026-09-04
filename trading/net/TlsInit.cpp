#include "trading/net/TlsInit.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace trading {

namespace {

// Converts a Windows CERT_CONTEXT to an OpenSSL X509 and adds it to the store.
bool addCertToStore(X509_STORE* store, PCCERT_CONTEXT cert) {
    const unsigned char* p = cert->pbCertEncoded;
    X509* x509 = d2i_X509(nullptr, &p, cert->cbCertEncoded);
    if (!x509) return false;
    bool ok = X509_STORE_add_cert(store, x509) == 1;
    X509_free(x509);
    return ok;
}

} // namespace

bool loadWindowsRootCerts(SSL_CTX* ctx) {
#ifdef _WIN32
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) return false;
    bool any = false;
    static const wchar_t* kStores[] = {L"ROOT", L"CA"};
    for (const wchar_t* name : kStores) {
        HCERTSTORE sys = CertOpenSystemStoreW(0, name);
        if (!sys) continue;
        PCCERT_CONTEXT cert = nullptr;
        while ((cert = CertEnumCertificatesInStore(sys, cert)) != nullptr) {
            if (addCertToStore(store, cert)) any = true;
        }
        if (cert) CertFreeCertificateContext(cert);
        CertCloseStore(sys, 0);
    }
    return any;
#else
    (void)ctx;
    return false;
#endif
}

SSL_CTX* createVerifiedClientCtx(std::string& error) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        error = "SSL_CTX_new failed";
        return nullptr;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (!loadWindowsRootCerts(ctx)) {
        // Fall back to the default path (rare on Windows, useful elsewhere).
        SSL_CTX_set_default_verify_paths(ctx);
    }
    return ctx;
}

} // namespace trading
