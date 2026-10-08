#include "tls/generate.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>

namespace houston_kvm::tls {

namespace {

constexpr size_t kMaxNames = 20;

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool isAddress(const std::string& s) {
    unsigned char buf[16];
    return inet_pton(AF_INET, s.c_str(), buf) == 1 || inet_pton(AF_INET6, s.c_str(), buf) == 1;
}

bool isHostName(std::string_view s) {
    if (s.starts_with("*.")) s.remove_prefix(2);
    if (s.empty() || s.size() > 253) return false;
    size_t start = 0;
    while (start <= s.size()) {
        auto dot = s.find('.', start);
        auto label = s.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (char c : label)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return true;
}

std::string opensslError(const char* what) {
    unsigned long e = ERR_get_error();
    char buf[256] = {};
    if (e) ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return std::string(what) + (e ? std::string(": ") + buf : std::string{});
}

std::shared_ptr<EVP_PKEY> newKey(KeyType type) {
    EVP_PKEY* key = nullptr;
    switch (type) {
    case KeyType::EcdsaP256: key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"); break;
    case KeyType::EcdsaP384: key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-384"); break;
    case KeyType::Rsa3072:   key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(3072)); break;
    }
    return {key, EVP_PKEY_free};
}

const EVP_MD* digestFor(KeyType type) {
    return type == KeyType::EcdsaP384 ? EVP_sha384() : EVP_sha256();
}

std::string pemOf(const std::function<int(BIO*)>& write) {
    std::shared_ptr<BIO> out(BIO_new(BIO_s_mem()), BIO_free);
    if (write(out.get()) != 1) return {};
    char* data = nullptr;
    long size = BIO_get_mem_data(out.get(), &data);
    return std::string(data, static_cast<size_t>(size));
}

std::string keyPem(EVP_PKEY* key) {
    return pemOf([key](BIO* b) { return PEM_write_bio_PrivateKey(b, key, nullptr, nullptr, 0, nullptr, nullptr); });
}

std::string subjectAltNames(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) out += ',';
        out += (isAddress(n) ? "IP:" : "DNS:") + n;
    }
    return out;
}

// Adds an extension written as in openssl.cnf. Only ever given values built
// here from checked names, so nothing from a request can add other syntax.
bool addExtension(X509* cert, X509V3_CTX* ctx, int nid, const std::string& value) {
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, ctx, nid, value.c_str());
    if (!ext) return false;
    bool ok = X509_add_ext(cert, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

// A new certificate with a random 127-bit serial, valid from a few minutes
// ago (clocks differ) for `days`.
std::shared_ptr<X509> newCertificate(EVP_PKEY* key, X509_NAME* subject, int days) {
    std::shared_ptr<X509> cert(X509_new(), X509_free);
    std::shared_ptr<BIGNUM> serial(BN_new(), BN_free);
    if (!cert || !serial || BN_rand(serial.get(), 127, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1) return nullptr;
    X509_set_version(cert.get(), 2);
    BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert.get()));
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300);
    X509_time_adj_ex(X509_getm_notAfter(cert.get()), days, 0, nullptr);
    X509_set_subject_name(cert.get(), subject);
    X509_set_pubkey(cert.get(), key);
    return cert;
}

std::shared_ptr<X509_NAME> nameWith(const std::string& commonName, const Subject& subject = {}) {
    std::shared_ptr<X509_NAME> name(X509_NAME_new(), X509_NAME_free);
    auto add = [&name](const char* field, const std::string& value) {
        if (!value.empty())
            X509_NAME_add_entry_by_txt(name.get(), field, MBSTRING_UTF8,
                                       reinterpret_cast<const unsigned char*>(value.c_str()), -1, -1, 0);
    };
    add("C", subject.country);
    add("O", subject.organization);
    add("OU", subject.unit);
    add("CN", commonName);
    return name;
}

std::shared_ptr<X509> readCert(std::string_view pem) {
    std::shared_ptr<BIO> in(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    return {PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr), X509_free};
}

int refusePassphrase(char*, int, int, void*) { return -1; }

std::shared_ptr<EVP_PKEY> readKey(std::string_view pem) {
    std::shared_ptr<BIO> in(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    return {PEM_read_bio_PrivateKey(in.get(), nullptr, refusePassphrase, nullptr), EVP_PKEY_free};
}

std::string checkSubjectField(const char* what, const std::string& value, size_t max) {
    if (value.size() > max) return std::string(what) + " is too long";
    for (unsigned char c : value)
        if (c < 0x20 || c == 0x7f) return std::string(what) + " has a control character";
    return "";
}

} // namespace

std::optional<KeyType> parseKeyType(std::string_view name) {
    if (name == "ecdsa-p256") return KeyType::EcdsaP256;
    if (name == "ecdsa-p384") return KeyType::EcdsaP384;
    if (name == "rsa-3072") return KeyType::Rsa3072;
    return std::nullopt;
}

const char* keyTypeName(KeyType type) {
    switch (type) {
    case KeyType::EcdsaP256: return "ecdsa-p256";
    case KeyType::EcdsaP384: return "ecdsa-p384";
    case KeyType::Rsa3072:   return "rsa-3072";
    }
    return "ecdsa-p256";
}

std::string checkNames(const std::vector<std::string>& names) {
    if (names.empty()) return "Give at least one name or address";
    if (names.size() > kMaxNames) return "At most " + std::to_string(kMaxNames) + " names";
    for (size_t i = 0; i < names.size(); ++i) {
        const auto& n = names[i];
        if (!isAddress(n) && !isHostName(n)) return "Not a host name or IP address: " + n.substr(0, 80);
        for (size_t j = 0; j < i; ++j)
            if (lower(names[j]) == lower(n)) return n + " is listed twice";
    }
    return "";
}

std::vector<std::string> suggestedNames() {
    std::vector<std::string> out;
    auto add = [&out](std::string n) {
        n = lower(n);
        if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end() &&
            (isAddress(n) || isHostName(n)))
            out.push_back(std::move(n));
    };
    add("localhost");
    char host[256] = {};
    if (gethostname(host, sizeof host - 1) == 0 && host[0]) {
        add(host);
        addrinfo hints{};
        hints.ai_flags = AI_CANONNAME;
        addrinfo* info = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &info) == 0) {
            if (info && info->ai_canonname) add(info->ai_canonname);
            freeaddrinfo(info);
        }
    }
    add("127.0.0.1");
    add("::1");
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (auto* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK)) continue;
            char buf[INET6_ADDRSTRLEN] = {};
            if (i->ifa_addr->sa_family == AF_INET) {
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof buf);
            } else if (i->ifa_addr->sa_family == AF_INET6) {
                auto* a = &reinterpret_cast<sockaddr_in6*>(i->ifa_addr)->sin6_addr;
                if (IN6_IS_ADDR_LINKLOCAL(a)) continue;
                inet_ntop(AF_INET6, a, buf, sizeof buf);
            } else {
                continue;
            }
            add(buf);
        }
        freeifaddrs(ifs);
    }
    return out;
}

std::optional<Made> makeCa(KeyType type, const std::string& commonName, int days, std::string& error) {
    auto key = newKey(type);
    if (!key) { error = opensslError("Couldn't make a key"); return std::nullopt; }
    Subject subject;
    subject.organization = "HoustonKVM";
    auto name = nameWith(commonName, subject);
    auto cert = newCertificate(key.get(), name.get(), days);
    if (!cert) { error = opensslError("Couldn't make the certificate"); return std::nullopt; }
    X509_set_issuer_name(cert.get(), name.get());
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, cert.get(), cert.get(), nullptr, nullptr, 0);
    if (!addExtension(cert.get(), &ctx, NID_basic_constraints, "critical,CA:TRUE,pathlen:0") ||
        !addExtension(cert.get(), &ctx, NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !addExtension(cert.get(), &ctx, NID_subject_key_identifier, "hash") ||
        X509_sign(cert.get(), key.get(), digestFor(type)) == 0) {
        error = opensslError("Couldn't sign the CA certificate");
        return std::nullopt;
    }
    return Made{pemOf([&cert](BIO* b) { return PEM_write_bio_X509(b, cert.get()); }), keyPem(key.get())};
}

std::optional<Made> issue(std::string_view caCertPem, std::string_view caKeyPem,
                          const std::vector<std::string>& names, KeyType type, int days,
                          std::string& error) {
    if (error = checkNames(names); !error.empty()) return std::nullopt;
    auto ca = readCert(caCertPem);
    auto caKey = readKey(caKeyPem);
    if (!ca || !caKey) { error = opensslError("Couldn't read the CA"); return std::nullopt; }
    auto key = newKey(type);
    if (!key) { error = opensslError("Couldn't make a key"); return std::nullopt; }
    auto subject = nameWith(names.front());
    auto cert = newCertificate(key.get(), subject.get(), days);
    if (!cert) { error = opensslError("Couldn't make the certificate"); return std::nullopt; }
    X509_set_issuer_name(cert.get(), X509_get_subject_name(ca.get()));
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, ca.get(), cert.get(), nullptr, nullptr, 0);
    std::string usage = type == KeyType::Rsa3072 ? "critical,digitalSignature,keyEncipherment"
                                                 : "critical,digitalSignature";
    int caBits = EVP_PKEY_get_bits(caKey.get());
    const EVP_MD* digest = EVP_PKEY_get_base_id(caKey.get()) == EVP_PKEY_EC && caBits > 256 ? EVP_sha384()
                                                                                              : EVP_sha256();
    if (!addExtension(cert.get(), &ctx, NID_basic_constraints, "critical,CA:FALSE") ||
        !addExtension(cert.get(), &ctx, NID_key_usage, usage) ||
        !addExtension(cert.get(), &ctx, NID_ext_key_usage, "serverAuth") ||
        !addExtension(cert.get(), &ctx, NID_subject_key_identifier, "hash") ||
        !addExtension(cert.get(), &ctx, NID_authority_key_identifier, "keyid:always") ||
        !addExtension(cert.get(), &ctx, NID_subject_alt_name, subjectAltNames(names)) ||
        X509_sign(cert.get(), caKey.get(), digest) == 0) {
        error = opensslError("Couldn't sign the certificate");
        return std::nullopt;
    }
    return Made{pemOf([&cert](BIO* b) { return PEM_write_bio_X509(b, cert.get()); }), keyPem(key.get())};
}

std::optional<Made> makeCsr(const std::vector<std::string>& names, KeyType type, const Subject& subject,
                            std::string& error) {
    if (error = checkNames(names); !error.empty()) return std::nullopt;
    if (error = checkSubjectField("Organization", subject.organization, 64); !error.empty()) return std::nullopt;
    if (error = checkSubjectField("Organizational unit", subject.unit, 64); !error.empty()) return std::nullopt;
    if (!subject.country.empty() &&
        (subject.country.size() != 2 || !std::isupper(static_cast<unsigned char>(subject.country[0])) ||
         !std::isupper(static_cast<unsigned char>(subject.country[1])))) {
        error = "Country must be two capital letters, like US";
        return std::nullopt;
    }
    auto key = newKey(type);
    if (!key) { error = opensslError("Couldn't make a key"); return std::nullopt; }
    std::shared_ptr<X509_REQ> req(X509_REQ_new(), X509_REQ_free);
    auto name = nameWith(names.front(), subject);
    X509_REQ_set_version(req.get(), 0);
    X509_REQ_set_subject_name(req.get(), name.get());
    X509_REQ_set_pubkey(req.get(), key.get());
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, nullptr, nullptr, req.get(), nullptr, 0);
    std::shared_ptr<STACK_OF(X509_EXTENSION)> exts(
        sk_X509_EXTENSION_new_null(), [](STACK_OF(X509_EXTENSION)* s) { sk_X509_EXTENSION_pop_free(s, X509_EXTENSION_free); });
    auto* san = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, subjectAltNames(names).c_str());
    if (!san || !sk_X509_EXTENSION_push(exts.get(), san) || X509_REQ_add_extensions(req.get(), exts.get()) != 1 ||
        X509_REQ_sign(req.get(), key.get(), digestFor(type)) == 0) {
        error = opensslError("Couldn't make the signing request");
        return std::nullopt;
    }
    return Made{pemOf([&req](BIO* b) { return PEM_write_bio_X509_REQ(b, req.get()); }), keyPem(key.get())};
}

bool issuedBy(std::string_view certPem, std::string_view caCertPem) {
    auto cert = readCert(certPem);
    auto ca = readCert(caCertPem);
    bool ok = cert && ca &&
              X509_NAME_cmp(X509_get_issuer_name(cert.get()), X509_get_subject_name(ca.get())) == 0 &&
              X509_verify(cert.get(), X509_get0_pubkey(ca.get())) == 1;
    ERR_clear_error();
    return ok;
}

} // namespace houston_kvm::tls
