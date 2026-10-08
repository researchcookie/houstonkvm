#include "tls/certificate.h"

#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>

namespace houston_kvm::tls {

namespace {

// Never prompt: with no callback, OpenSSL asks for a passphrase on the
// terminal.
int noPassphrase(char*, int, int, void*) { return -1; }

std::shared_ptr<BIO> memBio(std::string_view text) {
    return {BIO_new_mem_buf(text.data(), static_cast<int>(text.size())), BIO_free};
}

std::string nameText(const X509_NAME* name) {
    std::shared_ptr<BIO> out(BIO_new(BIO_s_mem()), BIO_free);
    X509_NAME_print_ex(out.get(), name, 0, XN_FLAG_RFC2253);
    char* data = nullptr;
    long size = BIO_get_mem_data(out.get(), &data);
    return std::string(data, static_cast<size_t>(size));
}

std::time_t asnTime(const ASN1_TIME* t) {
    struct tm tm {};
    if (ASN1_TIME_to_tm(t, &tm) != 1) return 0;
    return timegm(&tm);
}

std::string isoTime(std::time_t t) {
    struct tm tm {};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> altNames(X509* cert) {
    std::vector<std::string> out;
    auto* gens = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (!gens) return out;
    for (int i = 0; i < sk_GENERAL_NAME_num(gens); ++i) {
        const GENERAL_NAME* g = sk_GENERAL_NAME_value(gens, i);
        if (g->type == GEN_DNS) {
            auto* s = g->d.dNSName;
            out.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                             static_cast<size_t>(ASN1_STRING_length(s)));
        } else if (g->type == GEN_IPADD) {
            auto* s = g->d.iPAddress;
            char buf[INET6_ADDRSTRLEN] = {};
            int len = ASN1_STRING_length(s);
            int family = len == 4 ? AF_INET : len == 16 ? AF_INET6 : 0;
            if (family && inet_ntop(family, ASN1_STRING_get0_data(s), buf, sizeof buf)) out.emplace_back(buf);
        }
    }
    GENERAL_NAMES_free(gens);
    return out;
}

std::string fingerprint(X509* cert) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (X509_digest(cert, EVP_sha256(), md, &n) != 1) return {};
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned int i = 0; i < n; ++i) {
        if (i) out += ':';
        out += hex[md[i] >> 4];
        out += hex[md[i] & 0xF];
    }
    return out;
}

// "" with `type` set when the key is one browsers take, else why not.
std::string checkKeyType(EVP_PKEY* key, std::string& type) {
    switch (EVP_PKEY_get_base_id(key)) {
    case EVP_PKEY_RSA: {
        int bits = EVP_PKEY_get_bits(key);
        if (bits < 2048) return "An RSA key needs at least 2048 bits (this one has " + std::to_string(bits) + ")";
        type = "RSA " + std::to_string(bits);
        return "";
    }
    case EVP_PKEY_EC: {
        char group[64] = {};
        size_t len = 0;
        EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, &len);
        std::string_view g(group);
        if (g == "prime256v1" || g == "P-256") type = "ECDSA P-256";
        else if (g == "secp384r1" || g == "P-384") type = "ECDSA P-384";
        else return "An ECDSA key must use the P-256 or P-384 curve, which every browser supports";
        return "";
    }
    default:
        return "The key must be RSA or ECDSA";
    }
}

std::string normalizedAddress(std::string_view host) {
    std::string h(host);
    unsigned char buf[16];
    char out[INET6_ADDRSTRLEN];
    if (inet_pton(AF_INET, h.c_str(), buf) == 1 && inet_ntop(AF_INET, buf, out, sizeof out)) return out;
    if (inet_pton(AF_INET6, h.c_str(), buf) == 1 && inet_ntop(AF_INET6, buf, out, sizeof out)) return out;
    return {};
}

bool issuerIncluded(X509* leaf, STACK_OF(X509)* chain) {
    for (int i = 0; i < sk_X509_num(chain); ++i)
        if (X509_NAME_cmp(X509_get_subject_name(sk_X509_value(chain, i)), X509_get_issuer_name(leaf)) == 0)
            return true;
    return false;
}

// Whether the chain leads to a CA this machine trusts (the system bundle),
// for telling "signed by a well-known root, nothing missing" apart from a
// missing intermediate. A company's own root usually isn't in it.
bool verifiesAgainstSystemCAs(X509* leaf, STACK_OF(X509)* chain) {
    std::shared_ptr<X509_STORE> store(X509_STORE_new(), X509_STORE_free);
    if (!store || X509_STORE_set_default_paths(store.get()) != 1) return false;
    std::shared_ptr<X509_STORE_CTX> ctx(X509_STORE_CTX_new(), X509_STORE_CTX_free);
    if (!ctx || X509_STORE_CTX_init(ctx.get(), store.get(), leaf, chain) != 1) return false;
    return X509_verify_cert(ctx.get()) == 1;
}

} // namespace

nlohmann::json CertificateInfo::toJson(std::time_t now) const {
    return {
        {"subject", subject},
        {"issuer", issuer},
        {"self_signed", selfSigned},
        {"names", names},
        {"not_before", isoTime(notBefore)},
        {"not_after", isoTime(notAfter)},
        {"days_left", notAfter > now ? (notAfter - now) / 86400 : 0},
        {"expired", notAfter <= now},
        {"fingerprint_sha256", fingerprint},
        {"key_type", keyType},
    };
}

PairCheck loadPair(std::string_view certPem, std::string_view keyPem, bool requireCurrent,
                   std::time_t now) {
    PairCheck out;
    auto fail = [&out](std::string why) {
        ERR_clear_error();
        out.error = std::move(why);
        return out;
    };

    auto pair = std::make_shared<CertificatePair>();
    auto certBio = memBio(certPem);
    pair->leaf.reset(PEM_read_bio_X509(certBio.get(), nullptr, noPassphrase, nullptr), X509_free);
    if (!pair->leaf) return fail("The certificate isn't PEM text (-----BEGIN CERTIFICATE-----)");
    pair->chain.reset(sk_X509_new_null(), [](STACK_OF(X509)* s) { sk_X509_pop_free(s, X509_free); });
    while (X509* next = PEM_read_bio_X509(certBio.get(), nullptr, noPassphrase, nullptr))
        sk_X509_push(pair->chain.get(), next);
    ERR_clear_error();   // the read that ended the loop

    if (keyPem.find("ENCRYPTED") != std::string_view::npos)
        return fail("The key is protected by a passphrase, which the server has nowhere to keep: "
                    "remove it first (openssl pkey -in key.pem -out plain.pem)");
    auto keyBio = memBio(keyPem);
    pair->key.reset(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, noPassphrase, nullptr), EVP_PKEY_free);
    if (!pair->key) return fail("The key isn't a PEM private key (-----BEGIN PRIVATE KEY-----)");

    std::string keyType;
    if (auto e = checkKeyType(pair->key.get(), keyType); !e.empty()) return fail(e);
    if (X509_check_private_key(pair->leaf.get(), pair->key.get()) != 1)
        return fail("The key doesn't belong to this certificate");

    X509* leaf = pair->leaf.get();
    auto& info = pair->info;
    info.subject     = nameText(X509_get_subject_name(leaf));
    info.issuer      = nameText(X509_get_issuer_name(leaf));
    info.selfSigned  = X509_NAME_cmp(X509_get_subject_name(leaf), X509_get_issuer_name(leaf)) == 0;
    info.names       = altNames(leaf);
    info.notBefore   = asnTime(X509_get0_notBefore(leaf));
    info.notAfter    = asnTime(X509_get0_notAfter(leaf));
    info.fingerprint = fingerprint(leaf);
    info.keyType     = keyType;

    if (requireCurrent && info.notAfter <= now) return fail("The certificate has expired");
    if (requireCurrent && info.notBefore > now) return fail("The certificate isn't valid yet");

    if (info.names.empty())
        out.warnings.push_back("The certificate names no hosts (no subjectAltName): browsers will refuse it");
    if (!info.selfSigned && !issuerIncluded(leaf, pair->chain.get()) &&
        !verifiesAgainstSystemCAs(leaf, pair->chain.get())) {
        if (sk_X509_num(pair->chain.get()) == 0)
            out.warnings.push_back("Only the server's own certificate was given. If an intermediate CA "
                                   "issued it, add the intermediate after it: browsers often cope "
                                   "without it, but curl and other tools won't");
        else
            out.warnings.push_back("The chain doesn't include the certificate that issued this one (" +
                                   info.issuer + "). Browsers often cope, but curl and other tools "
                                   "will refuse it");
    }
    ERR_clear_error();
    out.pair = std::move(pair);
    return out;
}

bool coversName(const CertificateInfo& info, std::string_view host) {
    if (host.starts_with('[') && host.ends_with(']')) host = host.substr(1, host.size() - 2);
    auto address = normalizedAddress(host);
    auto h = lower(host);
    for (const auto& name : info.names) {
        if (!address.empty()) {
            if (normalizedAddress(name) == address) return true;
            continue;
        }
        auto n = lower(name);
        if (n == h) return true;
        if (n.starts_with("*.")) {
            auto dot = h.find('.');
            if (dot != std::string::npos && dot > 0 && h.substr(dot) == n.substr(1)) return true;
        }
    }
    return false;
}

std::optional<std::string> readFile(const std::string& path, size_t maxBytes, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": " + std::strerror(errno);
        return std::nullopt;
    }
    std::string data;
    data.resize(maxBytes + 1);
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(in.gcount()));
    if (data.size() > maxBytes) {
        error = path + ": larger than " + std::to_string(maxBytes) + " bytes";
        return std::nullopt;
    }
    return data;
}

bool writeFileAtomically(const std::string& path, std::string_view data, unsigned mode,
                         std::string& error) {
    std::string tmp = path + ".XXXXXX";
    int fd = mkstemp(tmp.data());
    if (fd < 0) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    bool ok = fchmod(fd, mode) == 0;
    for (size_t done = 0; ok && done < data.size();) {
        ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0;
        if (ok) done += static_cast<size_t>(n);
    }
    ok = ok && fsync(fd) == 0;
    int savedErrno = errno;
    close(fd);
    if (ok && rename(tmp.c_str(), path.c_str()) == 0) return true;
    if (ok) savedErrno = errno;
    unlink(tmp.c_str());
    error = path + ": " + std::strerror(savedErrno);
    return false;
}

} // namespace houston_kvm::tls
