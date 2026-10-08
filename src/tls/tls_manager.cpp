#include "tls/tls_manager.h"

#include "core/http_common.h"
#include "tls/generate.h"

#include <openssl/ssl.h>
#include <sodium.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <system_error>

#include <unistd.h>

namespace houston_kvm {

namespace {

constexpr size_t kMaxPemBytes = 64 * 1024;

const char* modeName(TlsManager::Mode m) {
    switch (m) {
    case TlsManager::Mode::Http:    return "http";
    case TlsManager::Mode::Pending: return "pending";
    case TlsManager::Mode::Https:   return "https";
    }
    return "http";
}

std::string portSuffix(int port, int defaultPort) {
    return port == defaultPort ? std::string{} : ":" + std::to_string(port);
}

TlsManager::Reply refuse(const char* status, std::string message) {
    return {status, std::move(message)};
}

constexpr const char* kPinnedMessage =
    "HTTPS is set by the server's configuration (--tls in /etc/houstonkvm/houstonkvm.conf)";
constexpr const char* kCertPinnedMessage =
    "The certificate is set by the server's configuration (/etc/houstonkvm/tls/ or --tls-cert)";

} // namespace

TlsManager::TlsManager(Config config, Database& db, EventBus& events,
                       std::function<void(uWS::App&)> registerRoutes,
                       std::function<void(uWS::SSLApp&)> registerSecureRoutes)
    : config_(std::move(config)), db_(db), events_(events), settings_(db.tlsSettings()) {
    if (config_.httpsPort) settings_.httpsPort = *config_.httpsPort;
    app_ = std::make_unique<uWS::App>();
    registerRoutes(*app_);

    // Sends every request to the same path on HTTPS. 307, not 308: browsers
    // keep a permanent redirect for good, and HTTPS may be turned off again.
    // HSTS is the deliberate way to make it stick.
    redirect_ = std::make_unique<uWS::App>();
    redirect_->any("/*", [this](auto* res, auto* req) {
        auto host = requestHost(req);
        if (host.empty()) {
            res->writeStatus("400 Bad Request")->end("Missing or invalid Host header");
            return;
        }
        res->writeStatus("307 Temporary Redirect")
           ->writeHeader("Location", "https://" + host + portSuffix(settings_.httpsPort, 443) +
                                     std::string(req->getFullUrl()))
           ->writeHeader("Cache-Control", "no-store")
           ->end();
    });

    // No certificate files: certCallback gives each handshake the pair in
    // use, which is what lets it change without a restart.
    secureApp_ = std::make_unique<uWS::SSLApp>();
    if (!secureApp_->constructorFailed()) {
        SSL_CTX_set_cert_cb(static_cast<SSL_CTX*>(secureApp_->getNativeHandle()), certCallback, this);
        registerSecureRoutes(*secureApp_);
    }

    // Lives as long as this does; armed only while Pending. Doesn't keep
    // the event loop running by itself.
    revertTimer_ = us_create_timer(reinterpret_cast<us_loop_t*>(uWS::Loop::get()), 1, sizeof(TlsManager*));
    *static_cast<TlsManager**>(us_timer_ext(revertTimer_)) = this;
    renewTimer_ = us_create_timer(reinterpret_cast<us_loop_t*>(uWS::Loop::get()), 1, sizeof(TlsManager*));
    *static_cast<TlsManager**>(us_timer_ext(renewTimer_)) = this;
}

TlsManager::~TlsManager() {
    if (revertTimer_) us_timer_close(revertTimer_);
    if (renewTimer_) us_timer_close(renewTimer_);
}

std::string TlsManager::certPath() const {
    return configManaged() ? config_.certFile : config_.stateDir + "/cert.pem";
}

std::string TlsManager::keyPath() const {
    return configManaged() ? config_.keyFile : config_.stateDir + "/key.pem";
}

std::shared_ptr<const tls::CertificatePair> TlsManager::loadFromFiles(std::string& error,
                                                                      std::string& certPem) const {
    auto cert = tls::readFile(certPath(), kMaxPemBytes, error);
    if (!cert) return nullptr;
    certPem = *cert;
    auto key = tls::readFile(keyPath(), kMaxPemBytes, error);
    if (!key) return nullptr;
    auto check = tls::loadPair(*cert, *key, false);
    if (!check.pair) {
        error = certPath() + ": " + check.error;
        return nullptr;
    }
    if (check.pair->info.notAfter <= std::time(nullptr))
        std::cerr << "HTTPS: the certificate in " << certPath() << " has expired\n";
    return check.pair;
}

int TlsManager::certCallback(SSL* ssl, void* arg) {
    auto* self = static_cast<TlsManager*>(arg);
    auto pair = self->pair_;
    if (!pair) return 0;   // fails the handshake
    return SSL_use_cert_and_key(ssl, pair->leaf.get(), pair->key.get(), pair->chain.get(), 1) == 1 ? 1 : 0;
}

us_listen_socket_t* TlsManager::listenPlain(uWS::App& app, int port) {
    // Exclusive: without it uSockets shares a port that's already taken
    // (SO_REUSEPORT), and two servers would split the requests between them.
    us_listen_socket_t* ls = nullptr;
    app.listen(config_.bindAddress, port, LIBUS_LISTEN_EXCLUSIVE_PORT, [&ls](auto* token) { ls = token; });
    return ls;
}

us_listen_socket_t* TlsManager::listenSecure(int port) {
    us_listen_socket_t* ls = nullptr;
    if (!secureApp_->constructorFailed())
        secureApp_->listen(config_.bindAddress, port, LIBUS_LISTEN_EXCLUSIVE_PORT, [&ls](auto* token) { ls = token; });
    return ls;
}

void TlsManager::closeListener(us_listen_socket_t*& ls, bool secure) {
    if (ls) us_listen_socket_close(secure ? 1 : 0, ls);
    ls = nullptr;
}

// The configuration's --https-port isn't the Owner's: what's stored stays
// theirs.
void TlsManager::save() {
    auto stored = settings_;
    if (config_.httpsPort) stored.httpsPort = db_.tlsSettings().httpsPort;
    db_.setTlsSettings(stored);
}

void TlsManager::emit(std::string_view type, std::string_view outcome, const Actor& who,
                      nlohmann::json detail) {
    Event e{type, outcome, who};
    e.detail = std::move(detail);
    events_.emit(e);
}

bool TlsManager::start(std::string& error) {
    std::string certPem;
    if (configManaged()) {
        usePair(loadFromFiles(error, certPem), certPem);
        if (!pair_) return false;   // never fall back to plain HTTP silently
    } else if (std::filesystem::exists(certPath())) {
        usePair(loadFromFiles(certError_, certPem), certPem);
        if (!pair_) std::cerr << "HTTPS: the certificate didn't load: " << certError_ << "\n";
    }
    us_timer_set(renewTimer_, [](us_timer_t* t) { (*static_cast<TlsManager**>(us_timer_ext(t)))->renewIfDue(); },
                 static_cast<int>(std::chrono::milliseconds(config_.renewCheck).count()),
                 static_cast<int>(std::chrono::milliseconds(config_.renewCheck).count()));

    if (config_.pinned == false && settings_.enabled) {
        // --tls=off is also the way back in after an HTTPS mistake. HTTPS
        // stays off once the option is removed again, or that would lock
        // the Owner out again; they turn it back on in Admin, where it's
        // confirm-or-revert.
        settings_.enabled = false;
        save();
        std::cout << "HTTPS: turned off by --tls=off, and stays off without it until an Owner turns it on\n";
        Actor configuration;
        configuration.username = "(--tls=off)";
        emit(event::kTlsDisabled, event::kOk, configuration, {{"https_port", settings_.httpsPort}});
    }
    bool https = config_.pinned.value_or(settings_.enabled);
    if (https && !pair_) {
        if (config_.pinned) {
            error = "--tls=on, but there's no certificate: put houstonkvm.crt and houstonkvm.key in "
                    "/etc/houstonkvm/tls/, or give --tls-cert and --tls-key";
            return false;
        }
        std::cerr << "HTTPS is on, but there's no usable certificate: serving plain HTTP until "
                     "an Owner installs one\n";
        https = false;
    }
    if (https) {
        secureListener_ = listenSecure(settings_.httpsPort);
        if (!secureListener_) {
            if (config_.pinned) {
                error = "Failed to bind " + config_.bindAddress + ":" + std::to_string(settings_.httpsPort) +
                        " for HTTPS";
                return false;
            }
            std::cerr << "HTTPS: failed to bind " << config_.bindAddress << ":" << settings_.httpsPort
                      << ", serving plain HTTP instead\n";
            https = false;
        }
    }

    if (https) {
        mode_ = Mode::Https;
        if (settings_.httpRedirect) {
            redirectListener_ = listenPlain(*redirect_, config_.httpPort);
            if (!redirectListener_)
                std::cerr << "HTTPS: failed to bind " << config_.bindAddress << ":" << config_.httpPort
                          << " for the redirect to HTTPS\n";
        }
        std::cout << config_.banner << " listening on https://" << config_.bindAddress << ":"
                  << settings_.httpsPort << "\n";
        return true;
    }
    appListener_ = listenPlain(*app_, config_.httpPort);
    if (!appListener_) {
        error = "Failed to bind " + config_.bindAddress + ":" + std::to_string(config_.httpPort);
        return false;
    }
    std::cout << config_.banner << " listening on http://" << config_.bindAddress << ":"
              << config_.httpPort << "\n";
    return true;
}

void TlsManager::stop() {
    endPending();
    us_timer_set(renewTimer_, [](us_timer_t*) {}, 0, 0);
    closeListener(appListener_, false);
    closeListener(redirectListener_, false);
    closeListener(secureListener_, true);
    app_->close();
    redirect_->close();
    if (!secureApp_->constructorFailed()) secureApp_->close();
}

TlsManager::Reply TlsManager::status(std::string_view host) const {
    nlohmann::json j = {
        {"mode", modeName(mode_)},
        {"pinned", config_.pinned ? nlohmann::json(*config_.pinned ? "on" : "off") : nlohmann::json(nullptr)},
        {"certificate_managed_by_config", configManaged()},
        {"http_port", config_.httpPort},
        {"https_port", settings_.httpsPort},
        {"https_port_managed_by_config", config_.httpsPort.has_value()},
        {"http_redirect", settings_.httpRedirect},
        {"hsts", settings_.hsts},
        {"auto_renew", settings_.autoRenew},
        {"certificate", nullptr},
        {"certificate_error", certError_},
        {"pending", nullptr},
    };
    auto now = std::time(nullptr);
    if (pair_) {
        j["certificate"] = pair_->info.toJson(now);
        j["certificate"]["covers_this_host"] = host.empty() || tls::coversName(pair_->info, host);
        j["certificate"]["issued_by_houstonkvm_ca"] = pairFromOurCa_;
    }
    j["ca"] = nullptr;
    if (auto ca = caCertificate()) {
        std::string unused;
        // The CA's own pair isn't loaded for this: describe its certificate.
        if (auto check = tls::loadPair(*ca, tls::readFile(caKeyPath(), kMaxPemBytes, unused).value_or(""), false);
            check.pair)
            j["ca"] = {{"subject", check.pair->info.subject},
                       {"fingerprint_sha256", check.pair->info.fingerprint},
                       {"not_after", check.pair->info.toJson(now)["not_after"]}};
    }
    j["pending_csr"] = std::filesystem::exists(csrPath());
    auto names = defaultNames(host);
    j["suggested_names"] = names;
    if (mode_ == Mode::Pending) {
        auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline_ - std::chrono::steady_clock::now());
        j["pending"] = {{"expires_in", std::max<int64_t>(0, left.count())}};
    }
    j["last_revert"] = nullptr;
    if (revertedAt_) {
        auto ago = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - *revertedAt_);
        j["last_revert"] = {{"seconds_ago", ago.count()}, {"https_port", settings_.httpsPort}};
    }
    return {"200 OK", std::move(j)};
}

TlsManager::Reply TlsManager::enable(const Actor& who, std::string_view host) {
    if (config_.pinned) return refuse("409 Conflict", kPinnedMessage);
    if (mode_ == Mode::Pending) return refuse("409 Conflict", "Already waiting for HTTPS to be confirmed");
    if (mode_ == Mode::Https) return refuse("409 Conflict", "HTTPS is already on");
    if (!pair_) return refuse("409 Conflict", "Install a certificate first");

    secureListener_ = listenSecure(settings_.httpsPort);
    if (!secureListener_)
        return refuse("409 Conflict", "Couldn't listen on port " + std::to_string(settings_.httpsPort) +
                                      ": another program has it, or it isn't permitted");

    unsigned char raw[16];
    randombytes_buf(raw, sizeof raw);
    char hex[sizeof raw * 2 + 1];
    sodium_bin2hex(hex, sizeof hex, raw, sizeof raw);
    code_ = hex;
    enabledBy_ = who;
    deadline_ = std::chrono::steady_clock::now() + config_.confirmWindow;
    us_timer_set(revertTimer_, [](us_timer_t* t) { (*static_cast<TlsManager**>(us_timer_ext(t)))->revert(); },
                 static_cast<int>(std::chrono::milliseconds(config_.confirmWindow).count()), 0);
    mode_ = Mode::Pending;
    revertedAt_.reset();
    std::cout << "HTTPS: listening on https://" << config_.bindAddress << ":" << settings_.httpsPort
              << ", waiting " << config_.confirmWindow.count() << " s to be confirmed\n";

    nlohmann::json warnings = nlohmann::json::array();
    if (!host.empty() && !tls::coversName(pair_->info, host))
        warnings.push_back("The certificate doesn't name " + std::string(host) +
                           ", the address in use, so the browser will warn about it");
    nlohmann::json body = {
        {"confirm_code", code_},
        {"expires_in", config_.confirmWindow.count()},
        {"warnings", std::move(warnings)},
    };
    if (!host.empty())
        body["https_url"] = "https://" + std::string(host) + portSuffix(settings_.httpsPort, 443) +
                            "/#tls-confirm=" + code_;
    return {"200 OK", std::move(body)};
}

void TlsManager::endPending() {
    if (revertTimer_) us_timer_set(revertTimer_, [](us_timer_t*) {}, 0, 0);
    code_.clear();
}

TlsManager::Reply TlsManager::confirm(std::string_view code, const std::string& ip) {
    if (mode_ != Mode::Pending) return refuse("409 Conflict", "Nothing is waiting to be confirmed");
    if (code.size() != code_.size() || sodium_memcmp(code.data(), code_.data(), code_.size()) != 0)
        return refuse("403 Forbidden", "Wrong confirmation code");

    endPending();
    mode_ = Mode::Https;
    settings_.enabled = true;
    save();

    // Everyone on plain HTTP comes back over HTTPS: their video streams and
    // input sockets close, and the page reconnects.
    closeListener(appListener_, false);
    app_->close();
    if (settings_.httpRedirect) {
        redirectListener_ = listenPlain(*redirect_, config_.httpPort);
        if (!redirectListener_)
            std::cerr << "HTTPS: failed to bind " << config_.bindAddress << ":" << config_.httpPort
                      << " for the redirect to HTTPS\n";
    }
    std::cout << "HTTPS: confirmed, plain HTTP "
              << (redirectListener_ ? "now redirects to HTTPS" : "is off") << "\n";
    emit(event::kTlsEnabled, event::kOk, enabledBy_,
         {{"confirmed_from", ip}, {"https_port", settings_.httpsPort},
          {"fingerprint_sha256", pair_->info.fingerprint}});
    return {"200 OK", {{"ok", true}}};
}

void TlsManager::revert() {
    if (mode_ != Mode::Pending) return;
    endPending();
    mode_ = Mode::Http;
    revertedAt_ = std::chrono::steady_clock::now();
    closeListener(secureListener_, true);
    secureApp_->close();
    std::cout << "HTTPS: not confirmed within " << config_.confirmWindow.count()
              << " s, back to plain HTTP only (is port " << settings_.httpsPort
              << "/tcp open in the firewall? firewall-cmd --list-all)\n";
    emit(event::kTlsReverted, event::kOk, enabledBy_,
         {{"https_port", settings_.httpsPort}, {"window_seconds", config_.confirmWindow.count()}});
}

TlsManager::Reply TlsManager::disable(const Actor& who) {
    if (config_.pinned) return refuse("409 Conflict", kPinnedMessage);
    if (mode_ == Mode::Http) return refuse("409 Conflict", "HTTPS is already off");

    bool wasPending = mode_ == Mode::Pending;
    if (wasPending) {
        endPending();
    } else {
        closeListener(redirectListener_, false);
        appListener_ = listenPlain(*app_, config_.httpPort);
        if (!appListener_) {
            if (settings_.httpRedirect) redirectListener_ = listenPlain(*redirect_, config_.httpPort);
            return refuse("500 Internal Server Error",
                          "Couldn't listen on the HTTP port " + std::to_string(config_.httpPort) +
                          " again, so HTTPS stays on");
        }
        redirect_->close();
        settings_.enabled = false;
        save();
    }
    mode_ = Mode::Http;
    closeListener(secureListener_, true);
    // The request asking for this may have come over HTTPS: answer it first.
    uWS::Loop::get()->defer([this] {
        if (mode_ == Mode::Http) secureApp_->close();
    });
    std::cout << "HTTPS: turned off, serving plain HTTP\n";
    emit(event::kTlsDisabled, event::kOk, who, {{"while_waiting_for_confirmation", wasPending}});
    return {"200 OK", {{"ok", true}, {"http_port", config_.httpPort}}};
}

TlsManager::Reply TlsManager::installCertificate(const Actor& who, std::string_view certPem,
                                                 std::string_view keyPem, std::string_view host) {
    if (configManaged()) return refuse("409 Conflict", kCertPinnedMessage);
    if (!keyPem.empty()) return install(who, certPem, keyPem, "upload", host);

    // The certificate the Owner's CA issued for the pending CSR.
    std::string error;
    auto key = tls::readFile(csrKeyPath(), kMaxPemBytes, error);
    if (!key)
        return refuse("400 Bad Request", "Give the key too: there's no signing request waiting for a certificate");
    auto reply = install(who, certPem, *key, "csr", host);
    if (std::string_view(reply.status).starts_with("200")) {
        std::error_code ec;
        std::filesystem::remove(csrPath(), ec);
        std::filesystem::remove(csrKeyPath(), ec);
    }
    return reply;
}

TlsManager::Reply TlsManager::install(const Actor& who, std::string_view certPem, std::string_view keyPem,
                                      std::string_view source, std::string_view host,
                                      std::vector<std::string> warnings) {
    auto check = tls::loadPair(certPem, keyPem, true);
    if (!check.pair) {
        if (source == "csr" && check.error == "The key doesn't belong to this certificate")
            return refuse("400 Bad Request", "This certificate wasn't issued for the pending signing request");
        return refuse("400 Bad Request", check.error);
    }

    std::error_code ec;
    std::filesystem::create_directories(config_.stateDir, ec);
    std::filesystem::permissions(config_.stateDir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    std::string error;
    if (!tls::writeFileAtomically(keyPath(), keyPem, 0600, error) ||
        !tls::writeFileAtomically(certPath(), certPem, 0644, error)) {
        std::cerr << "HTTPS: couldn't store the certificate: " << error << "\n";
        return refuse("500 Internal Server Error", "Couldn't store the certificate: " + error);
    }
    usePair(check.pair, certPem);
    certError_.clear();

    const auto& info = pair_->info;
    warnings.insert(warnings.end(), check.warnings.begin(), check.warnings.end());
    if (!host.empty() && !tls::coversName(info, host))
        warnings.push_back("The certificate doesn't name " + std::string(host) +
                           ", the address in use, so browsers will warn about it");
    std::cout << "HTTPS: certificate installed (" << source << ", " << info.subject << ", "
              << info.fingerprint << ")\n";
    emit(event::kTlsCertInstalled, event::kOk, who,
         {{"source", source}, {"subject", info.subject}, {"names", info.names},
          {"self_signed", info.selfSigned}, {"fingerprint_sha256", info.fingerprint},
          {"not_after", info.toJson(0)["not_after"]}});
    auto described = info.toJson(std::time(nullptr));
    described["issued_by_houstonkvm_ca"] = pairFromOurCa_;
    return {"200 OK", {{"certificate", std::move(described)}, {"warnings", std::move(warnings)}}};
}

void TlsManager::usePair(std::shared_ptr<const tls::CertificatePair> pair, std::string_view certPem) {
    pair_ = std::move(pair);
    auto ca = caCertificate();
    pairFromOurCa_ = pair_ && ca && tls::issuedBy(certPem, *ca);
}

std::optional<std::string> TlsManager::caCertificate() const {
    std::string error;
    return std::filesystem::exists(caCertPath()) ? tls::readFile(caCertPath(), kMaxPemBytes, error)
                                                 : std::nullopt;
}

std::optional<std::string> TlsManager::pendingCsr() const {
    std::string error;
    return std::filesystem::exists(csrPath()) ? tls::readFile(csrPath(), kMaxPemBytes, error) : std::nullopt;
}

// What the browser is using first, then everything this machine is called.
std::vector<std::string> TlsManager::defaultNames(std::string_view host) const {
    std::vector<std::string> names;
    std::string h(host);
    if (h.size() > 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    if (!h.empty()) names.push_back(h);
    for (auto& n : tls::suggestedNames())
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(std::move(n));
    if (names.size() > 20) names.resize(20);
    return names;
}

namespace {

// {names, key_type} from a request: nullopt with `error` set when either is
// there but wrong.
bool namesAndKeyType(const nlohmann::json& request, std::vector<std::string>& names,
                     tls::KeyType& type, std::string& error) {
    if (!request.is_object()) { error = "Expected a JSON object"; return false; }
    if (auto it = request.find("names"); it != request.end()) {
        if (!it->is_array()) { error = "names must be a list"; return false; }
        names.clear();
        for (const auto& n : *it) {
            if (!n.is_string()) { error = "names must be text"; return false; }
            names.push_back(n.get<std::string>());
        }
    }
    if (auto it = request.find("key_type"); it != request.end()) {
        auto parsed = it->is_string() ? tls::parseKeyType(it->get<std::string>()) : std::nullopt;
        if (!parsed) { error = "key_type must be ecdsa-p256, ecdsa-p384 or rsa-3072"; return false; }
        type = *parsed;
    }
    error = tls::checkNames(names);
    return error.empty();
}

std::optional<tls::KeyType> keyTypeOf(const std::string& described) {
    if (described == "ECDSA P-256") return tls::KeyType::EcdsaP256;
    if (described == "ECDSA P-384") return tls::KeyType::EcdsaP384;
    if (described == "RSA 3072") return tls::KeyType::Rsa3072;
    return std::nullopt;
}

} // namespace

TlsManager::Reply TlsManager::generate(const Actor& who, const nlohmann::json& request, std::string_view host) {
    if (configManaged()) return refuse("409 Conflict", kCertPinnedMessage);
    auto names = defaultNames(host);
    auto type = tls::KeyType::EcdsaP256;
    std::string error;
    if (!namesAndKeyType(request, names, type, error)) return refuse("400 Bad Request", error);

    std::error_code ec;
    std::filesystem::create_directories(config_.stateDir + "/ca", ec);
    std::filesystem::permissions(config_.stateDir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::permissions(config_.stateDir + "/ca", std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    auto ca = caCertificate();
    std::optional<std::string> caKey;
    if (ca) caKey = tls::readFile(caKeyPath(), kMaxPemBytes, error);
    if (!ca || !caKey) {
        char hostname[256] = {};
        gethostname(hostname, sizeof hostname - 1);
        auto made = tls::makeCa(type, std::string("HoustonKVM CA on ") + (hostname[0] ? hostname : "this server"),
                                config_.caDays, error);
        if (!made || !tls::writeFileAtomically(caKeyPath(), made->keyPem, 0600, error) ||
            !tls::writeFileAtomically(caCertPath(), made->certPem, 0644, error)) {
            std::cerr << "HTTPS: couldn't make the CA: " << error << "\n";
            return refuse("500 Internal Server Error", "Couldn't make the CA: " + error);
        }
        ca = made->certPem;
        caKey = made->keyPem;
        auto described = tls::loadPair(*ca, *caKey, false);
        std::cout << "HTTPS: made HoustonKVM's own CA\n";
        emit(event::kTlsCaCreated, event::kOk, who,
             {{"subject", described.pair ? described.pair->info.subject : ""},
              {"fingerprint_sha256", described.pair ? described.pair->info.fingerprint : ""}});
    }
    auto issued = tls::issue(*ca, *caKey, names, type, config_.certDays, error);
    if (!issued) {
        std::cerr << "HTTPS: couldn't issue a certificate: " << error << "\n";
        return refuse("500 Internal Server Error", error);
    }
    // The CA goes in the chain: no warning about it missing, and a browser's
    // certificate viewer can show it.
    return install(who, issued->certPem + *ca, issued->keyPem, "houstonkvm-ca", host);
}

void TlsManager::renewIfDue() {
    if (!settings_.autoRenew || configManaged() || !pair_ || !pairFromOurCa_) return;
    auto left = pair_->info.notAfter - std::time(nullptr);
    if (left > static_cast<std::time_t>(config_.renewDaysBefore) * 86400) return;

    Actor system;
    system.username = "(auto-renew)";
    auto type = keyTypeOf(pair_->info.keyType).value_or(tls::KeyType::EcdsaP256);
    nlohmann::json request = {{"names", pair_->info.names}, {"key_type", tls::keyTypeName(type)}};
    auto reply = generate(system, request, "");
    if (!std::string_view(reply.status).starts_with("200")) {
        auto why = reply.body.is_string() ? reply.body.get<std::string>() : reply.body.dump();
        std::cerr << "HTTPS: renewing the certificate failed: " << why << "\n";
        emit(event::kTlsCertInstalled, event::kFailed, system, {{"source", "renewal"}, {"error", why}});
    }
}

TlsManager::Reply TlsManager::createCsr(const Actor& who, const nlohmann::json& request) {
    if (configManaged()) return refuse("409 Conflict", kCertPinnedMessage);
    std::vector<std::string> names;
    auto type = tls::KeyType::EcdsaP256;
    std::string error;
    if (!namesAndKeyType(request, names, type, error)) return refuse("400 Bad Request", error);
    tls::Subject subject;
    for (auto [key, field] : {std::pair{"organization", &subject.organization},
                              std::pair{"unit", &subject.unit}, std::pair{"country", &subject.country}}) {
        auto it = request.find(key);
        if (it == request.end()) continue;
        if (!it->is_string()) return refuse("400 Bad Request", std::string(key) + " must be text");
        *field = it->get<std::string>();
    }
    auto made = tls::makeCsr(names, type, subject, error);
    if (!made) return refuse("400 Bad Request", error);

    std::error_code ec;
    std::filesystem::create_directories(config_.stateDir + "/pending", ec);
    std::filesystem::permissions(config_.stateDir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::permissions(config_.stateDir + "/pending", std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    if (!tls::writeFileAtomically(csrKeyPath(), made->keyPem, 0600, error) ||
        !tls::writeFileAtomically(csrPath(), made->certPem, 0644, error))
        return refuse("500 Internal Server Error", "Couldn't store the signing request: " + error);
    std::cout << "HTTPS: made a certificate signing request for " << names.front() << "\n";
    emit(event::kTlsCsrCreated, event::kOk, who, {{"names", names}, {"key_type", tls::keyTypeName(type)}});
    return {"200 OK", {{"csr", made->certPem}}};
}

TlsManager::Reply TlsManager::discardCsr(const Actor& who) {
    if (!std::filesystem::exists(csrPath())) return refuse("404 Not Found", "No signing request is waiting");
    std::error_code ec;
    std::filesystem::remove(csrPath(), ec);
    std::filesystem::remove(csrKeyPath(), ec);
    (void)who;
    return {"200 OK", {{"ok", true}}};
}

TlsManager::Reply TlsManager::updateSettings(const Actor& who, const nlohmann::json& changes) {
    if (!changes.is_object()) return refuse("400 Bad Request", "Expected a JSON object");
    if (mode_ == Mode::Pending)
        return refuse("409 Conflict", "Wait until HTTPS is confirmed or turned off again");

    auto next = settings_;
    for (const auto& [key, value] : changes.items()) {
        if (key == "https_port") {
            if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 65535)
                return refuse("400 Bad Request", "https_port must be 1-65535");
            next.httpsPort = value.get<int>();
            if (config_.httpsPort && next.httpsPort != *config_.httpsPort)
                return refuse("409 Conflict", "The HTTPS port is set by the server's configuration (--https-port)");
            if (next.httpsPort == config_.httpPort)
                return refuse("400 Bad Request", "https_port must differ from the HTTP port");
        } else if (key == "http_redirect" || key == "hsts" || key == "auto_renew") {
            if (!value.is_boolean()) return refuse("400 Bad Request", key + " must be true or false");
            (key == "hsts" ? next.hsts : key == "auto_renew" ? next.autoRenew : next.httpRedirect) =
                value.get<bool>();
        } else {
            return refuse("400 Bad Request", "Unknown setting: " + key);
        }
    }
    if (next.hsts && !settings_.hsts && (!pair_ || pair_->info.selfSigned))
        return refuse("409 Conflict", "HSTS needs a certificate from a certificate authority: it takes "
                                      "away the browser's way past a certificate warning");

    if (mode_ == Mode::Https) {
        bool redirectOn = next.httpRedirect && !settings_.httpRedirect;
        if (redirectOn) {
            redirectListener_ = listenPlain(*redirect_, config_.httpPort);
            if (!redirectListener_)
                return refuse("409 Conflict", "Couldn't listen on the HTTP port " +
                                              std::to_string(config_.httpPort) + " for the redirect");
        }
        if (next.httpsPort != settings_.httpsPort) {
            // Open connections stay where they are; new ones go to the new port.
            auto* ls = listenSecure(next.httpsPort);
            if (!ls) {
                if (redirectOn) closeListener(redirectListener_, false);
                return refuse("409 Conflict", "Couldn't listen on port " + std::to_string(next.httpsPort) +
                                              ": another program has it, or it isn't permitted");
            }
            closeListener(secureListener_, true);
            secureListener_ = ls;
        }
        if (!next.httpRedirect && settings_.httpRedirect) closeListener(redirectListener_, false);
    }

    nlohmann::json changed = nlohmann::json::object();
    if (next.httpsPort != settings_.httpsPort)
        changed["https_port"] = {{"from", settings_.httpsPort}, {"to", next.httpsPort}};
    if (next.httpRedirect != settings_.httpRedirect)
        changed["http_redirect"] = {{"from", settings_.httpRedirect}, {"to", next.httpRedirect}};
    if (next.hsts != settings_.hsts)
        changed["hsts"] = {{"from", settings_.hsts}, {"to", next.hsts}};
    if (next.autoRenew != settings_.autoRenew)
        changed["auto_renew"] = {{"from", settings_.autoRenew}, {"to", next.autoRenew}};
    settings_ = next;
    save();
    if (!changed.empty()) emit(event::kTlsSettings, event::kOk, who, {{"changes", std::move(changed)}});
    return status("");
}

void TlsManager::reload() {
    if (!configManaged() && !std::filesystem::exists(certPath())) {
        std::cout << "HTTPS: reload: no certificate installed\n";
        return;
    }
    std::string error, certPem;
    auto pair = loadFromFiles(error, certPem);
    Actor system;
    system.username = "(reload)";
    if (!pair) {
        std::cerr << "HTTPS: reload failed, keeping the certificate in use: " << error << "\n";
        certError_ = "The last reload failed: " + error;
        emit(event::kTlsReloaded, event::kFailed, system, {{"error", error}});
        return;
    }
    usePair(pair, certPem);
    certError_.clear();
    std::cout << "HTTPS: certificate reloaded (" << pair_->info.fingerprint << ")\n";
    emit(event::kTlsReloaded, event::kOk, system,
         {{"fingerprint_sha256", pair_->info.fingerprint}, {"not_after", pair_->info.toJson(0)["not_after"]}});
}

} // namespace houston_kvm
