#include <App.h>
#include <libusockets.h>

#include "auth/auth.h"
#include "core/audit_log.h"
#include "core/database.h"
#include "core/events.h"
#include "core/http_common.h"
#include "core/json_log.h"
#include "core/metrics.h"
#include "core/routes_audit.h"
#include "core/routes_targets.h"
#include "core/target_manager.h"
#include "auth/password_hasher.h"
#include "auth/rate_limiter.h"
#include "auth/routes_auth.h"
#include "tls/routes_tls.h"
#include "tls/tls_manager.h"
#include "hid/routes_health.h"
#include "hid/routes_input.h"
#include "video/routes_settings.h"
#include "video/routes_stream.h"
#include "webrtc/routes_webrtc.h"
#include "webrtc/selective_forwarding_unit.h"
#include "ui_bundle.h"

#include <rtc/rtc.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

using namespace houston_kvm;

int main(int argc, char* argv[]) {
    // Under systemd stdout is a pipe, which the C library buffers in full: a
    // line printed without std::endl reached the journal only when something
    // later flushed it. Line-buffered, each line goes out when it ends.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    int         port          = 8080;
    std::string bindAddress   = "0.0.0.0";
    std::string dbPath        = "houstonkvm.db";
    bool        secureCookies = false;
    int         auditRetentionDays = 90;
    RequestPolicy policy;
    std::vector<IceServerConfig> iceServers;
    bool        noIceServers  = false;
    std::optional<bool> tlsPinned;
    std::string tlsCertFile, tlsKeyFile;
    std::optional<int> httpsPort;
    // Hidden, for tests: where the configuration's certificate is looked
    // for, how long an HTTPS switch waits to be confirmed, how long the own
    // CA's certificates last, and how often renewal is checked.
    std::string tlsConfigDir  = "/etc/houstonkvm/tls";
    int         tlsConfirmSeconds = 120;
    int         tlsCertDays = 397;
    int         tlsRenewCheckSeconds = 3600;
    bool        jsonLogs      = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg.starts_with("--port=")) {
            try {
                size_t used = 0;
                port = std::stoi(std::string(arg.substr(7)), &used);
                if (used != arg.size() - 7 || port < 1 || port > 65535) throw std::out_of_range("port");
            } catch (const std::exception&) {
                std::cerr << "Invalid port in " << arg << " (expected 1-65535)\n";
                return 2;
            }
        } else if (arg.starts_with("--bind="))
            bindAddress = std::string(arg.substr(7));
        else if (arg.starts_with("--db="))
            dbPath = std::string(arg.substr(5));
        else if (arg == "--secure-cookies")
            secureCookies = true;
        else if (arg == "--log-format=json")
            jsonLogs = true;
        else if (arg == "--log-format=text")
            jsonLogs = false;
        else if (arg.starts_with("--audit-retention-days=")) {
            try {
                size_t used = 0;
                auto value = std::string(arg.substr(23));
                auditRetentionDays = std::stoi(value, &used);
                if (used != value.size() || auditRetentionDays < 1 || auditRetentionDays > 36500)
                    throw std::out_of_range("days");
            } catch (const std::exception&) {
                std::cerr << "Invalid value in " << arg << " (expected 1-36500 days)\n";
                return 2;
            }
        }
        else if (arg.starts_with("--allowed-origin=")) {
            auto origin = std::string(arg.substr(17));
            if (!origin.starts_with("http://") && !origin.starts_with("https://")) {
                std::cerr << "Invalid origin in " << arg << " (expected e.g. https://kvm.example.com)\n";
                return 2;
            }
            while (origin.ends_with('/')) origin.pop_back();
            policy.allowedOrigins.push_back(std::move(origin));
        } else if (arg.starts_with("--trusted-proxy="))
            policy.trustedProxies.emplace_back(arg.substr(16));
        else if (arg.starts_with("--ice-server=")) {
            auto url = std::string(arg.substr(13));
            std::string error;
            if (url == "none") {
                noIceServers = true;
            } else if (auto server = parseIceServerArg(url, error)) {
                iceServers.push_back(std::move(*server));
            } else {
                std::cerr << "Invalid ICE server in " << arg << ": " << error
                          << " (expected stun:HOST[:PORT], turn:USER:PASS@HOST[:PORT] or none)\n";
                return 2;
            }
        }
        else if (arg == "--tls=on" || arg == "--tls=off")
            tlsPinned = arg == "--tls=on";
        else if (arg.starts_with("--https-port=")) {
            try {
                size_t used = 0;
                auto value = std::string(arg.substr(13));
                httpsPort = std::stoi(value, &used);
                if (used != value.size() || *httpsPort < 1 || *httpsPort > 65535) throw std::out_of_range("port");
            } catch (const std::exception&) {
                std::cerr << "Invalid port in " << arg << " (expected 1-65535)\n";
                return 2;
            }
        }
        else if (arg.starts_with("--tls-cert="))
            tlsCertFile = std::string(arg.substr(11));
        else if (arg.starts_with("--tls-key="))
            tlsKeyFile = std::string(arg.substr(10));
        else if (arg.starts_with("--tls-config-dir="))
            tlsConfigDir = std::string(arg.substr(17));
        else if (arg.starts_with("--tls-confirm-seconds=") || arg.starts_with("--tls-cert-days=") ||
                 arg.starts_with("--tls-renew-check-seconds=")) {
            auto eq = arg.find('=');
            auto name = arg.substr(0, eq);
            int& target = name == "--tls-confirm-seconds" ? tlsConfirmSeconds
                        : name == "--tls-cert-days"       ? tlsCertDays
                                                          : tlsRenewCheckSeconds;
            try {
                size_t used = 0;
                auto value = std::string(arg.substr(eq + 1));
                target = std::stoi(value, &used);
                if (used != value.size() || target < 1 || target > 3650) throw std::out_of_range("value");
            } catch (const std::exception&) {
                std::cerr << "Invalid value in " << arg << " (expected 1-3650)\n";
                return 2;
            }
        }
        else if (arg == "--version") {
            std::cout << "HoustonKVM " << HOUSTONKVM_VERSION << "\n";
            return 0;
        } else if (arg == "--help") {
            std::cout << "Usage: houstonkvm [--port=8080] [--bind=0.0.0.0] [--db=houstonkvm.db]\n"
                          "                  [--secure-cookies] [--trusted-proxy=ADDR]...\n"
                          "                  [--allowed-origin=URL]... [--audit-retention-days=90]\n"
                          "                  [--ice-server=URL|none]... [--tls=on|off]\n"
                          "                  [--https-port=8443] [--tls-cert=FILE --tls-key=FILE]\n"
                          "                  [--log-format=text|json]\n"
                          "                  [--version]\n"
                          "--bind sets the address to listen on; use --bind=127.0.0.1 when a\n"
                          "reverse proxy on the same machine terminates TLS in front of this server.\n"
                          "--trusted-proxy names a proxy (e.g. 127.0.0.1) whose X-Forwarded-For\n"
                          "header is believed, so sign-in rate limiting sees each visitor's own\n"
                          "address instead of the proxy's. Repeat it for several proxies.\n"
                          "--allowed-origin lets pages from that origin (e.g.\n"
                          "https://kvm.example.com) use the session cookie; only needed behind a\n"
                          "proxy that doesn't pass the browser's Host header through.\n"
                          "--ice-server names a STUN or TURN server for low-latency (WebRTC)\n"
                          "video (stun:HOST[:PORT], turn:USER:PASS@HOST[:PORT]); repeat it for\n"
                          "several, or give --ice-server=none for none. The server and every\n"
                          "viewer's browser use the same list. Without this option an Owner sets\n"
                          "the list under Admin -> Network; it starts empty, since on a LAN or\n"
                          "VPN the server's own addresses are all a browser needs. With it, the\n"
                          "list is fixed here and Admin shows it read-only.\n"
                          "Everything else is configured at runtime and stored in the\n"
                          "database: each target's capture device/resolution/fps, QEMU socket,\n"
                          "serial device/baud and WebRTC bitrate, plus its name, group and tags,\n"
                          "via the /api/targets API.\n"
                          "--audit-retention-days is how long the audit log (sign-ins, account and\n"
                          "target changes, who drove which target and for how long) is kept.\n"
                          "--secure-cookies marks the plain-HTTP session cookie Secure; only use\n"
                          "it when a reverse proxy terminates TLS in front of this server,\n"
                          "otherwise browsers will silently refuse to store it.\n"
                          "HTTPS: an Owner turns it on under Admin -> Network (or POST\n"
                          "/api/tls/enable), and installs its certificate there. Plain HTTP\n"
                          "until then. --tls=on or --tls=off fixes it either way instead;\n"
                          "--tls=off is also the way back in after an HTTPS mistake.\n"
                          "--https-port fixes the HTTPS port (otherwise the Owner's, 8443 to start).\n"
                          "--tls-cert and --tls-key name a certificate chain and its key (PEM)\n"
                          "to serve; without them, /etc/houstonkvm/tls/houstonkvm.crt and\n"
                          "houstonkvm.key are used when both exist. Either way Admin can't\n"
                          "replace that certificate, and SIGHUP (systemctl reload) re-reads it.\n"
                          "--log-format=json writes each log line as a JSON object (ts, level,\n"
                          "component, msg), for a log shipper. Metrics are at GET /metrics.\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << " (try --help)\n";
            return 2;
        }
    }

    if (jsonLogs) enableJsonLogs();   // before any other thread starts

    // libdatachannel logs nothing unless asked. HOUSTONKVM_RTC_LOG=debug (or
    // verbose, info, warning, error) turns its log on, for diagnosing
    // low-latency video that won't connect.
    if (const char* level = std::getenv("HOUSTONKVM_RTC_LOG")) {
        std::string_view l(level);
        auto rtcLevel = l == "verbose" ? rtc::LogLevel::Verbose
                      : l == "debug"   ? rtc::LogLevel::Debug
                      : l == "info"    ? rtc::LogLevel::Info
                      : l == "warning" ? rtc::LogLevel::Warning
                                       : rtc::LogLevel::Error;
        rtc::InitLogger(rtcLevel, [](rtc::LogLevel, std::string message) {
            std::cerr << "libdatachannel: " << message << std::endl;
        });
    }

    if (httpsPort && *httpsPort == port) {
        std::cerr << "--https-port must differ from --port\n";
        return 2;
    }
    if (tlsCertFile.empty() != tlsKeyFile.empty()) {
        std::cerr << "--tls-cert and --tls-key go together\n";
        return 2;
    }
    if (tlsCertFile.empty()) {
        // The configuration's certificate, when it's in the usual place.
        auto crt = tlsConfigDir + "/houstonkvm.crt", key = tlsConfigDir + "/houstonkvm.key";
        std::error_code ec;
        bool hasCrt = std::filesystem::exists(crt, ec), hasKey = std::filesystem::exists(key, ec);
        if (hasCrt != hasKey) {
            std::cerr << "Fatal: " << (hasCrt ? key : crt) << " is missing (" << (hasCrt ? crt : key)
                      << " is there): HTTPS needs both\n";
            return 1;
        }
        if (hasCrt) {
            tlsCertFile = crt;
            tlsKeyFile = key;
        }
    }

    if (noIceServers && !iceServers.empty()) {
        std::cerr << "--ice-server=none can't be combined with other --ice-server options\n";
        return 2;
    }

    // Block SIGINT/SIGTERM here, before any other thread is created, so
    // every thread spawned below (each target's StreamManager and InputQueue
    // workers, the shutdownWatcher thread started further down) inherits the
    // blocked mask. shutdownWatcher is the only thread that ever unblocks
    // and waits on these via sigwait().
    sigset_t shutdownSignals;
    sigemptyset(&shutdownSignals);
    sigaddset(&shutdownSignals, SIGINT);
    sigaddset(&shutdownSignals, SIGTERM);
    sigaddset(&shutdownSignals, SIGHUP);   // not shutdown: re-reads the HTTPS certificate
    pthread_sigmask(SIG_BLOCK, &shutdownSignals, nullptr);

    Database db(dbPath);
    if (!db.isOpen()) {
        std::cerr << "Fatal: cannot open database '" << dbPath << "': " << db.openError() << "\n";
        return 1;
    }
    db.init();
    db.migrate();

    Auth auth;
    auth.policy = std::move(policy);
    LoginRateLimiter loginLimiter;

    // Created (and so owned by) this thread, which is also the one that runs
    // the event loop below. Capture threads defer frame delivery onto it.
    uWS::Loop* loop = uWS::Loop::get();

    // Declared before sfu so it is destroyed after every peer connection is
    // gone. libdatachannel keeps global worker threads that only rtc::Cleanup()
    // joins; 0.19 (EPEL 9's system copy) aborts at exit if they're still
    // running, on every exit path including a failed listen.
    struct RtcCleanup {
        ~RtcCleanup() { rtc::Cleanup().wait_for(std::chrono::seconds(5)); }
    } rtcCleanup;

    // Any --ice-server fixes the list; otherwise it's the Owner's, from the
    // database. Nothing by default: the server makes no connection out of
    // the network that nobody asked for.
    const bool iceServersPinned = noIceServers || !iceServers.empty();
    if (!iceServersPinned) {
        std::string error;
        auto stored = nlohmann::json::parse(db.iceServersJson(), nullptr, false);
        if (auto servers = iceServersFromJson(stored, error)) iceServers = std::move(*servers);
        else std::cerr << "Ignoring the stored STUN/TURN servers: " << error << "\n";
    }

    // Placeholder bitrate. Each target's StreamManager pushes its own
    // configured bitrate into sfu.setBitrateKbps() when it (re)builds, so
    // the SFU currently tracks whichever target was configured last — the
    // bitrate is global to the SFU, not yet per-room.
    SelectiveForwardingUnit sfu(4000, std::move(iceServers));

    // Every auditable action is reported here once; the audit log records
    // it (and a metrics endpoint can count the same stream).
    EventBus events;
    AuditLog audit(db, loop, auditRetentionDays);
    events.subscribe([&audit](const Event& e) { audit.record(e); });
    Metrics metrics(loop);
    events.subscribe([&metrics](const Event& e) { metrics.count(e); });

    // Declared after sfu and db so it is destroyed first: destroying it
    // joins every target's worker threads, which call back into sfu.
    // Declared after audit too, whose control hooks it calls.
    TargetManager targets(sfu, db, loop);
    auto targetName = [&db](int64_t id) {
        auto row = db.getTarget(id);
        return row ? row->name : std::string{};
    };
    targets.setControlHooks(ControlHooks{
        .started = [&audit, &events, targetName](const TargetRuntime& t, const Actor& who) {
            Event e{event::kControlAcquired, event::kOk, who};
            e.targetId = t.id;
            e.targetName = targetName(t.id);
            int64_t session = audit.beginControl(t.id, e.targetName, who);
            e.detail = {{"control_session", session}};
            events.emit(e);
            return session;
        },
        .keyPressed = [&audit](int64_t session) { audit.keyPressed(session); },
        .mouse = [&audit](int64_t session) { audit.mouse(session); },
        .ended = [&audit, &events, targetName](const TargetRuntime& t, int64_t session,
                                               std::string_view reason) {
            audit.endControl(session, reason);
            Event e{event::kControlReleased, event::kOk, t.driver};
            e.targetId = t.id;
            e.targetName = targetName(t.id);
            e.detail = {{"control_session", session}, {"reason", reason}};
            events.emit(e);
        },
    });
    targets.loadAll();

    {
        DbOutcome outcome;
        int purged = auth.purgeExpiredSessions(db, outcome);
        if (!outcome.ok) std::cerr << "Session purge failed: " << outcome.message << "\n";
        else if (purged > 0) std::cout << "Removed " << purged << " expired sessions\n";
    }

    // Declared last of the long-lived objects so it is destroyed first: its
    // destructor joins the hashing threads, whose finished jobs refer to
    // auth, db and the rate limiter.
    PasswordHasher hasher(loop);

    // The UI can control a real machine's keyboard and mouse, so it must never
    // be embedded in another site's page (clickjacking), must run no script
    // or style but its own files (so injected markup stays inert), and
    // browsers should not guess types or leak the address in a Referer header.
    // blob: carries snapshots; WebRTC media arrives outside CSP's reach.
    static constexpr const char* kContentSecurityPolicy =
        "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' blob: data:; "
        "connect-src 'self'; media-src 'self' blob:; object-src 'none'; base-uri 'none'; "
        "form-action 'self'; frame-ancestors 'none'";
    TlsManager* tls = nullptr;   // set below, before anything listens
    // no-cache makes the browser ask before reusing its copy; the ETag (a
    // hash of the file, from the build) lets it ask cheaply, so a file it
    // already has costs a 304 with no body, and an upgrade shows at once.
    auto serveEmbedded = [&tls](auto* res, auto* req, const EmbeddedFile& f) {
        const bool unchanged = etagListMatches(req->getHeader("if-none-match"), f.etag);
        if (unchanged) res->writeStatus("304 Not Modified");
        if constexpr (std::is_same_v<decltype(res), uWS::HttpResponse<true>*>)
            if (tls->hsts()) res->writeHeader("Strict-Transport-Security", "max-age=31536000");
        if (!unchanged) res->writeHeader("Content-Type", f.mimeType);
        res->writeHeader("Cache-Control", "no-cache")
           ->writeHeader("ETag", f.etag)
           ->writeHeader("X-Frame-Options", "DENY")
           ->writeHeader("Content-Security-Policy", kContentSecurityPolicy)
           ->writeHeader("X-Content-Type-Options", "nosniff")
           ->writeHeader("Referrer-Policy", "no-referrer");
        if (unchanged) res->endWithoutBody();
        else res->end(f.content);
    };
    // Plain HTTP and HTTPS serve the same routes, each from its own app.
    auto registerRoutes = [&](auto& app) {
        for (const auto& f : EMBEDDED_FILES) {
            app.get(std::string(f.path), [f, serveEmbedded](auto* res, auto* req) { serveEmbedded(res, req, f); });
            if (f.path == "/index.html")
                app.get("/", [f, serveEmbedded](auto* res, auto* req) { serveEmbedded(res, req, f); });
        }

        registerAuthRoutes(app, auth, db, targets, loginLimiter, hasher, events, secureCookies);
        registerTargetRoutes(app, auth, db, targets, events);
        registerSettingsRoutes(app, auth, db, targets, events);
        registerWebrtcRoutes(app, auth, db, sfu, targets, events, iceServersPinned);
        registerStreamRoutes(app, auth, db, targets);
        registerInputRoutes(app, auth, db, targets, events);
        registerHealthRoutes(app, auth, db, targets, events);
        registerAuditRoutes(app, auth, db, audit);
        registerTlsRoutes(app, auth, db, tls);
        registerMetricsRoutes(app, auth, db, targets, metrics);
    };

    TlsManager::Config tlsConfig;
    tlsConfig.banner        = std::string("HoustonKVM ") + HOUSTONKVM_VERSION;
    tlsConfig.bindAddress   = bindAddress;
    tlsConfig.httpPort      = port;
    tlsConfig.pinned        = tlsPinned;
    tlsConfig.httpsPort     = httpsPort;
    tlsConfig.certFile      = tlsCertFile;
    tlsConfig.keyFile       = tlsKeyFile;
    // Beside the database, the one place the service may write.
    tlsConfig.stateDir      = (std::filesystem::path(dbPath).parent_path() / "tls").string();
    tlsConfig.confirmWindow = std::chrono::seconds(tlsConfirmSeconds);
    tlsConfig.certDays      = tlsCertDays;
    tlsConfig.renewCheck    = std::chrono::seconds(tlsRenewCheckSeconds);
    TlsManager tlsManager(std::move(tlsConfig), db, events,
                          [&registerRoutes](uWS::App& app) { registerRoutes(app); },
                          [&registerRoutes](uWS::SSLApp& app) { registerRoutes(app); });
    tls = &tlsManager;

    {
        Stmt users(db.handle(), "SELECT COUNT(*) FROM users");
        if (users.step() == Stmt::StepResult::Row && users.column_int(0) == 0) {
            // Flushed: stdout to the journal is block-buffered, and whoever
            // installed the server is waiting for this line. Built first and
            // written in one go: std::cerr is tied to std::cout, so another
            // thread's error line would otherwise flush the first half of this
            // one on its own and land in the middle of the code.
            std::cout << std::string("No accounts exist yet. To create the Owner account, open the web UI\n"
                                     "and enter this setup code: ") + auth.issueSetupCode() + "\n"
                      << std::flush;
        }
    }

    if (std::string error; !tlsManager.start(error)) {
        // The port is taken, the address isn't ours, we lack permission, or
        // the configuration's HTTPS can't be served. Exit non-zero rather
        // than idle: under systemd a process that is up but not listening
        // looks like a healthy service.
        std::cerr << error << "\n";
        tlsManager.stop();
        targets.closeAllStreams();
        targets.stopBackground();
        return 1;
    }

    audit.start();
    metrics.start();

    // Notices a driver whose session expired, whose API token was revoked,
    // whose account was deleted or who lost the Operator role, and releases
    // their control (see TargetManager::startDriverRevalidation). Bounded
    // staleness: at most one interval. A database error counts as "still
    // valid" — a transient hiccup mustn't yank control mid-task.
    targets.startDriverRevalidation([&auth, &db](const std::string& token) {
        DbOutcome outcome;
        auto user = auth.verifySession(db, token, outcome);
        if (!user && outcome.ok) user = auth.verifyApiToken(db, token, outcome);
        if (!outcome.ok) return true;
        return user && user->role == Role::Operator;
    }, 2000);

    // Waits for SIGINT/SIGTERM/SIGHUP (blocked for every thread above, so
    // only this dedicated thread ever observes them via sigwait).
    // uWS::Loop::get() is lazily created per-thread, so we can't call it
    // again from this thread to reach the real event loop — defer() on the
    // pointer captured earlier (`loop`) is the thread-safe way in.
    std::thread shutdownWatcher([&shutdownSignals, loop, &targets, &audit, &metrics, &tlsManager] {
        int sig = 0;
        while (sigwait(&shutdownSignals, &sig) == 0 && sig == SIGHUP)
            loop->defer([&tlsManager] { tlsManager.reload(); });
        std::cout << "\nHoustonKVM received shutdown signal, stopping...\n";
        loop->defer([&targets, &audit, &metrics, &tlsManager] {
            tlsManager.stop();
            // /api/stream clients are held open indefinitely; force-close
            // them so the loop actually runs out of work and run() returns.
            targets.closeAllStreams();
            targets.stopBackground();
            audit.stop();   // records every driver's session as ended here
            metrics.stop();
        });
    });

    loop->run();

    shutdownWatcher.join();
    std::cout << "HoustonKVM stopped\n";

    return 0;
}
