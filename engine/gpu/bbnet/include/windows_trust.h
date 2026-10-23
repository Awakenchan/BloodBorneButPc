// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: HTTPS servers Windows trusts are trusted here too. OpenSSL checks the server's chain
// against the Windows ROOT and CA stores as they are when the game starts; a root Windows only
// fetches on demand (ISRG Root X1 on a fresh install) or the certificate an antivirus' HTTPS
// scanning signs the connection with (Avast, AVG, Kaspersky) can fail that check while the
// launcher, browsers and the rest of Windows connect ("SSL server verification failed",
// "Failed to acquire Bloodborne server information"). When OpenSSL rejects the chain, the same
// certificate and host name go through Windows' own chain building and SSL policy
// (CertGetCertificateChain, which fetches missing roots and intermediates): accepted when
// Windows accepts it, rejected otherwise. BB_WINDOWS_TRUST=0: OpenSSL's answer only.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <httplib.h>

namespace bbnet {

inline std::string HostOf(const std::string& url) {
    std::string host = url;
    if (const auto scheme = host.find("://"); scheme != std::string::npos) host.erase(0, scheme + 3);
    if (const auto end = host.find_first_of("/?#"); end != std::string::npos) host.erase(end);
    if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
    if (!host.empty() && host[0] == '[') {
        if (const auto close = host.find(']'); close != std::string::npos) return host.substr(1, close - 1);
    }
    if (const auto colon = host.rfind(':'); colon != std::string::npos) host.erase(colon);
    return host;
}

inline void TrustWindowsRoots(httplib::Client& client, const std::string& url) {
#if defined(_WIN32) && defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    static const bool enabled = [] {
        const char* env = std::getenv("BB_WINDOWS_TRUST");
        return !(env && env[0] == '0');
    }();
    if (!enabled) return;
    client.set_session_verifier([host = HostOf(url)](httplib::tls::session_t session) {
        if (httplib::tls::get_verify_result(session) == 0) {
            return httplib::SSLVerifierResponse::NoDecisionMade;
        }
        auto cert = httplib::tls::get_peer_cert(session);
        if (!cert) return httplib::SSLVerifierResponse::NoDecisionMade;
        std::vector<unsigned char> der;
        const bool have_der = httplib::tls::get_cert_der(cert, der);
        httplib::tls::free_cert(cert);
        uint64_t error = 0;
        if (have_der && httplib::detail::verify_cert_with_windows_schannel(der, host, true, error)) {
            static std::atomic<int> noted{0};
            if (noted.fetch_add(1) < 3) {
                std::printf("Online: %s's certificate is trusted by Windows (not by the bundled TLS "
                            "library's copy of its stores): connecting\n",
                            host.c_str());
                std::fflush(stdout);
            }
            return httplib::SSLVerifierResponse::CertificateAccepted;
        }
        static std::atomic<int> refused{0};
        if (refused.fetch_add(1) < 3) {
            std::printf("Online: %s's certificate is not trusted by Windows either (error %#llx)\n",
                        host.c_str(), static_cast<unsigned long long>(error));
            std::fflush(stdout);
        }
        return httplib::SSLVerifierResponse::CertificateRejected;
    });
#else
    (void)client;
    (void)url;
#endif
}

} // namespace bbnet
