#include "tls/routes_tls.h"

#include "core/http_common.h"

#include <string>

namespace houston_kvm {

namespace {

constexpr size_t kMaxCertificateBody = 64 * 1024;

template <bool SSL>
void respond(uWS::HttpResponse<SSL>* res, const TlsManager::Reply& reply) {
    res->writeStatus(reply.status);
    if (reply.body.is_string()) {
        res->end(reply.body.template get<std::string>());
        return;
    }
    res->writeHeader("Content-Type", "application/json")->end(reply.body.dump());
}

} // namespace

// `tls` is set once the manager exists, before the server listens.
template <bool SSL>
void registerTlsRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TlsManager*& tls) {

app.get("/api/tls", [&auth, &db, &tls](auto* res, auto* req) {
    if (!requireRole(auth, db, res, req, Role::Owner)) return;
    respond(res, tls->status(requestHost(req)));
});

app.put("/api/tls/settings", [&auth, &db, &tls](auto* res, auto* req) {
    auto who = requireRole(auth, db, res, req, Role::Owner);
    if (!who) return;
    auto actor = actorOf(*who, clientAddress(auth.policy, res, req));
    readBody(res, 4096, [res, &tls, actor](std::string& body) {
        auto j = parseJsonOr400(res, body);
        if (!j) return;
        respond(res, tls->updateSettings(actor, *j));
    });
});

app.post("/api/tls/certificate", [&auth, &db, &tls](auto* res, auto* req) {
    auto who = requireRole(auth, db, res, req, Role::Owner);
    if (!who) return;
    auto actor = actorOf(*who, clientAddress(auth.policy, res, req));
    auto host = requestHost(req);
    readBody(res, kMaxCertificateBody, [res, &tls, actor, host](std::string& body) {
        auto j = parseJsonOr400(res, body);
        if (!j) return;
        auto cert = j->is_object() ? jsonValueOr(*j, "cert", std::string{}) : std::nullopt;
        auto key = j->is_object() ? jsonValueOr(*j, "key", std::string{}) : std::nullopt;
        if (!cert || !key || cert->empty()) {
            res->writeStatus("400 Bad Request")->end("Expected {\"cert\": PEM, \"key\": PEM}");
            return;
        }
        respond(res, tls->installCertificate(actor, *cert, *key, host));
    });
});

app.post("/api/tls/generate", [&auth, &db, &tls](auto* res, auto* req) {
    auto who = requireRole(auth, db, res, req, Role::Owner);
    if (!who) return;
    auto actor = actorOf(*who, clientAddress(auth.policy, res, req));
    auto host = requestHost(req);
    readBody(res, 16 * 1024, [res, &tls, actor, host](std::string& body) {
        auto j = body.empty() ? std::optional<json>(json::object()) : parseJsonOr400(res, body);
        if (!j) return;
        respond(res, tls->generate(actor, *j, host));
    });
});

app.post("/api/tls/csr", [&auth, &db, &tls](auto* res, auto* req) {
    auto who = requireRole(auth, db, res, req, Role::Owner);
    if (!who) return;
    auto actor = actorOf(*who, clientAddress(auth.policy, res, req));
    readBody(res, 16 * 1024, [res, &tls, actor](std::string& body) {
        auto j = parseJsonOr400(res, body);
        if (!j) return;
        respond(res, tls->createCsr(actor, *j));
    });
});

app.get("/api/tls/csr", [&auth, &db, &tls](auto* res, auto* req) {
    if (!requireRole(auth, db, res, req, Role::Owner)) return;
    auto csr = tls->pendingCsr();
    if (!csr) {
        res->writeStatus("404 Not Found")->end("No signing request is waiting");
        return;
    }
    res->writeHeader("Content-Type", "application/pkcs10")
       ->writeHeader("Content-Disposition", "attachment; filename=\"houstonkvm.csr\"")
       ->end(*csr);
});

app.del("/api/tls/csr", [&auth, &db, &tls](auto* res, auto* req) {
    auto who = requireRole(auth, db, res, req, Role::Owner);
    if (!who) return;
    respond(res, tls->discardCsr(actorOf(*who, clientAddress(auth.policy, res, req))));
});

// Everyone who uses the server may need to trust its CA, so any signed-in
// user can fetch it. A certificate is public.
app.get("/api/tls/ca.crt", [&auth, &db, &tls](auto* res, auto* req) {
    if (!requireRole(auth, db, res, req, Role::Viewer)) return;
    auto ca = tls->caCertificate();
    if (!ca) {
        res->writeStatus("404 Not Found")->end("HoustonKVM hasn't made its own CA");
        return;
    }
    res->writeHeader("Content-Type", "application/x-x509-ca-cert")
       ->writeHeader("Content-Disposition", "attachment; filename=\"houstonkvm-ca.crt\"")
       ->end(*ca);
});

app.post("/api/tls/enable", [&auth, &db, &tls](auto* res, auto* req) {
    auto who = requireRole(auth, db, res, req, Role::Owner);
    if (!who) return;
    respond(res, tls->enable(actorOf(*who, clientAddress(auth.policy, res, req)), requestHost(req)));
});

app.post("/api/tls/disable", [&auth, &db, &tls](auto* res, auto* req) {
    auto who = requireRole(auth, db, res, req, Role::Owner);
    if (!who) return;
    respond(res, tls->disable(actorOf(*who, clientAddress(auth.policy, res, req))));
});

// No sign-in: the code is the proof, and all it can do is keep a change an
// Owner already started. The HTTPS page has no session yet anyway: the
// plain-HTTP cookie isn't honoured there.
app.post("/api/tls/confirm", [&auth, &tls](auto* res, auto* req) {
    if constexpr (!SSL) {
        res->writeStatus("409 Conflict")->end("Confirm over HTTPS, at the address /api/tls/enable gave");
        (void)auth; (void)tls; (void)req;
    } else {
        auto ip = clientAddress(auth.policy, res, req);
        readBody(res, 1024, [res, &tls, ip](std::string& body) {
            auto j = parseJsonOr400(res, body);
            if (!j) return;
            auto code = j->is_object() ? jsonValueOr(*j, "code", std::string{}) : std::nullopt;
            if (!code) {
                res->writeStatus("400 Bad Request")->end("Expected {\"code\": ...}");
                return;
            }
            respond(res, tls->confirm(*code, ip));
        });
    }
});

}

// Plain HTTP and HTTPS run the same routes.
template void registerTlsRoutes<false>(uWS::App&, Auth&, Database&, TlsManager*&);
template void registerTlsRoutes<true>(uWS::SSLApp&, Auth&, Database&, TlsManager*&);

} // namespace houston_kvm
