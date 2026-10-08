#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace houston_kvm::tls {

// Keys and certificates made on the server, through OpenSSL's EVP API only,
// so it works the same in FIPS mode.

enum class KeyType { EcdsaP256, EcdsaP384, Rsa3072 };

// "ecdsa-p256" (the default), "ecdsa-p384" or "rsa-3072".
std::optional<KeyType> parseKeyType(std::string_view name);
const char* keyTypeName(KeyType type);

// "" when every entry is a host name (a "*." wildcard included) or an IP
// address and there are 1 to 20 of them, else what's wrong, for the Owner.
std::string checkNames(const std::vector<std::string>& names);

// The names this machine is reached by: localhost, its host name and
// fully qualified name, and its addresses (loopback included, link-local
// IPv6 left out). Lowercase, no duplicates.
std::vector<std::string> suggestedNames();

struct Made {
    std::string certPem;   // or the CSR, for makeCsr
    std::string keyPem;
};

// A certificate authority: `commonName`, valid `days`, allowed to issue
// server certificates only (pathlen 0).
std::optional<Made> makeCa(KeyType type, const std::string& commonName, int days, std::string& error);

// A server certificate for `names`, issued by the CA, valid `days`.
std::optional<Made> issue(std::string_view caCertPem, std::string_view caKeyPem,
                          const std::vector<std::string>& names, KeyType type, int days,
                          std::string& error);

// Optional subject fields some company CAs insist on.
struct Subject {
    std::string organization;
    std::string unit;
    std::string country;   // two letters
};

// A key and a certificate signing request for `names` (the first is the
// common name), for the Owner's own CA to sign.
std::optional<Made> makeCsr(const std::vector<std::string>& names, KeyType type, const Subject& subject,
                            std::string& error);

// Whether `certPem`'s first certificate was signed by the CA in `caCertPem`.
bool issuedBy(std::string_view certPem, std::string_view caCertPem);

} // namespace houston_kvm::tls
