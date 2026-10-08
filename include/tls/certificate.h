#pragma once

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace houston_kvm::tls {

// What the Owner sees about a certificate: never anything from the key.
struct CertificateInfo {
    std::string              subject;       // RFC 2253, e.g. "CN=kvm.example.com,O=Example"
    std::string              issuer;
    bool                     selfSigned = false;
    std::vector<std::string> names;         // subjectAltName DNS names and IP addresses
    std::time_t              notBefore = 0;
    std::time_t              notAfter = 0;
    std::string              fingerprint;   // SHA-256, "AB:CD:..."
    std::string              keyType;       // "ECDSA P-256", "RSA 3072"

    nlohmann::json toJson(std::time_t now) const;
};

// A certificate chain and its private key, parsed and checked against each
// other: what a TLS handshake is served. Immutable once loaded, so a
// handshake can keep using one while a newer one replaces it.
struct CertificatePair {
    std::shared_ptr<X509>            leaf;
    std::shared_ptr<EVP_PKEY>        key;
    std::shared_ptr<STACK_OF(X509)>  chain;   // the certificates after the leaf
    CertificateInfo                  info;
};

struct PairCheck {
    std::shared_ptr<const CertificatePair> pair;   // null when refused
    std::string                            error;  // why, worded for the Owner
    std::vector<std::string>               warnings;
};

// Parses PEM text: a certificate chain (leaf first) and an unencrypted
// private key. Refused: anything that doesn't parse, a key that doesn't
// match, an encrypted key (there's nowhere to keep a passphrase), and keys
// other than RSA of 2048 bits or more and ECDSA. `requireCurrent` also
// refuses a certificate that has expired or isn't valid yet: on install,
// not when re-reading what's already in use, where serving it is better
// than serving nothing. Warns when the chain looks incomplete.
PairCheck loadPair(std::string_view certPem, std::string_view keyPem, bool requireCurrent,
                   std::time_t now = std::time(nullptr));

// True when `host` (a name or address, no port) is one of the
// certificate's names, a "*." name covering one label included.
bool coversName(const CertificateInfo& info, std::string_view host);

// A file's contents, refusing anything over maxBytes. nullopt with `error`
// set when it can't be read.
std::optional<std::string> readFile(const std::string& path, size_t maxBytes, std::string& error);

// Writes `data` to a temporary file beside `path` with `mode`, then renames
// it over `path`, so a reader sees the old file or the new one, never half.
bool writeFileAtomically(const std::string& path, std::string_view data, unsigned mode,
                         std::string& error);

} // namespace houston_kvm::tls
