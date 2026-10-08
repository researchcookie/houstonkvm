#pragma once

#include "core/database.h"
#include "core/events.h"
#include "tls/certificate.h"

#include <App.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace houston_kvm {

// Serves the web UI and API over plain HTTP, or HTTPS, switching at runtime
// without a restart. Three apps share the event loop, each
// listening only while it's in use:
//   - the full plain app on the HTTP port, while HTTPS is off or waiting
//     to be confirmed;
//   - the full TLS app on the HTTPS port, while HTTPS is on or waiting;
//   - a redirect-only plain app on the HTTP port, once HTTPS is on (if
//     the Owner keeps the redirect).
//
// Turning HTTPS on is confirm-or-revert, so it can't lock the Owner out:
// enable() starts HTTPS beside HTTP and returns a one-time code; only
// confirm(), with that code, over HTTPS, makes it stick. Unconfirmed, it is
// torn down when the window ends.
//
// The certificate comes from files: the Owner's, installed through the API
// into <state dir>/tls/, or the server's configuration's (--tls-cert and
// --tls-key, or /etc/houstonkvm/tls/), which the API then can't replace.
// The Owner's can be uploaded, issued from a CSR by their own CA, or issued
// by HoustonKVM's own small CA (<state dir>/tls/ca/), made on first use:
// trusting that CA once lets its certificates renew themselves without
// anyone having to trust a new one.
// Every handshake is given the pair in use at that moment, so a new one
// (installed, or re-read on SIGHUP) applies to new connections at once, and
// open ones keep theirs.
//
// Everything here runs on the event-loop thread.
class TlsManager {
public:
    enum class Mode { Http, Pending, Https };

    struct Config {
        std::string banner = "HoustonKVM";   // starts the "listening on" line
        std::string bindAddress = "0.0.0.0";
        int         httpPort = 8080;
        std::optional<bool> pinned;          // --tls=on|off: HTTPS on or off, not the Owner's choice
        std::optional<int>  httpsPort;       // --https-port: not the Owner's choice either
        std::string certFile, keyFile;       // set: the configuration's certificate
        std::string stateDir;                // the Owner's certificate: <stateDir>/cert.pem, key.pem
        std::chrono::seconds confirmWindow{120};
        int         caDays = 3650;           // HoustonKVM's own CA
        int         certDays = 397;          // what it issues; Apple refuses longer
        int         renewDaysBefore = 30;
        std::chrono::seconds renewCheck{3600};
    };

    // An API answer: the status line, and a JSON body, or a string sent as
    // plain text.
    struct Reply {
        const char*    status;
        nlohmann::json body;
    };

    TlsManager(Config config, Database& db, EventBus& events,
               std::function<void(uWS::App&)> registerRoutes,
               std::function<void(uWS::SSLApp&)> registerSecureRoutes);
    ~TlsManager();
    TlsManager(const TlsManager&) = delete;
    TlsManager& operator=(const TlsManager&) = delete;

    // Loads the certificate and listens as last saved. false, with `error`
    // set, when the server mustn't start: the HTTP port can't be had, or
    // the configuration asks for HTTPS that can't be served (a certificate
    // file it names is wrong, or --tls=on with no certificate).
    bool start(std::string& error);

    // Closes every listener and connection, so the event loop can end.
    void stop();

    // For the routes in routes_tls.h. `host` is the request's Host header
    // without the port: the name the Owner's browser is using.
    Reply status(std::string_view host) const;
    Reply enable(const Actor& who, std::string_view host);
    Reply confirm(std::string_view code, const std::string& ip);
    Reply disable(const Actor& who);
    // keyPem empty: the key of the pending CSR.
    Reply installCertificate(const Actor& who, std::string_view certPem, std::string_view keyPem,
                             std::string_view host);
    Reply updateSettings(const Actor& who, const nlohmann::json& changes);
    // {names, key_type}, both optional: a certificate from HoustonKVM's own
    // CA, made first if there isn't one yet, installed at once.
    Reply generate(const Actor& who, const nlohmann::json& request, std::string_view host);
    // {names, key_type, organization, unit, country}: a key kept on the
    // server, and a CSR for it, replacing any earlier one.
    Reply createCsr(const Actor& who, const nlohmann::json& request);
    Reply discardCsr(const Actor& who);
    std::optional<std::string> pendingCsr() const;
    std::optional<std::string> caCertificate() const;

    // SIGHUP: re-reads the certificate files. A pair that doesn't load
    // leaves the one in use in place.
    void reload();

    Mode mode() const { return mode_; }
    bool hsts() const { return mode_ == Mode::Https && settings_.hsts; }

private:
    bool configManaged() const { return !config_.certFile.empty(); }
    std::string certPath() const;
    std::string keyPath() const;
    std::string caCertPath() const { return config_.stateDir + "/ca/ca.crt"; }
    std::string caKeyPath() const { return config_.stateDir + "/ca/ca.key"; }
    std::string csrPath() const { return config_.stateDir + "/pending/request.csr"; }
    std::string csrKeyPath() const { return config_.stateDir + "/pending/key.pem"; }
    void usePair(std::shared_ptr<const tls::CertificatePair> pair, std::string_view certPem);
    Reply install(const Actor& who, std::string_view certPem, std::string_view keyPem,
                  std::string_view source, std::string_view host, std::vector<std::string> warnings = {});
    std::vector<std::string> defaultNames(std::string_view host) const;
    void renewIfDue();
    // Reads and checks the pair in use; sets certError_ when it won't load.
    std::shared_ptr<const tls::CertificatePair> loadFromFiles(std::string& error, std::string& certPem) const;

    us_listen_socket_t* listenPlain(uWS::App& app, int port);
    us_listen_socket_t* listenSecure(int port);
    void closeListener(us_listen_socket_t*& ls, bool secure);
    void save();
    void endPending();
    void revert();
    void emit(std::string_view type, std::string_view outcome, const Actor& who, nlohmann::json detail);

    static int certCallback(SSL* ssl, void* self);

    Config                         config_;
    Database&                      db_;
    EventBus&                      events_;
    Database::TlsSettings          settings_;
    Mode                           mode_ = Mode::Http;

    std::unique_ptr<uWS::App>      app_;          // the full plain app
    std::unique_ptr<uWS::App>      redirect_;
    std::unique_ptr<uWS::SSLApp>   secureApp_;
    us_listen_socket_t*            appListener_ = nullptr;
    us_listen_socket_t*            redirectListener_ = nullptr;
    us_listen_socket_t*            secureListener_ = nullptr;

    std::shared_ptr<const tls::CertificatePair> pair_;
    bool                           pairFromOurCa_ = false;
    std::string                    certError_;    // why the certificate files didn't load
    us_timer_t*                    renewTimer_ = nullptr;

    // While Pending.
    std::string                    code_;
    Actor                          enabledBy_;
    std::chrono::steady_clock::time_point deadline_;
    us_timer_t*                    revertTimer_ = nullptr;
    // When the last switch went back for want of a confirmation, so Admin
    // can say why once the Owner is back on plain HTTP.
    std::optional<std::chrono::steady_clock::time_point> revertedAt_;
};

} // namespace houston_kvm
