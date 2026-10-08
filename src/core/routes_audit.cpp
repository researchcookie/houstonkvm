#include "core/routes_audit.h"

#include "core/http_common.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace houston_kvm {

using json = nlohmann::json;

namespace {

std::string query(uWS::HttpRequest* req, std::string_view key) {
    auto v = req->getQuery(key);
    return v.empty() ? std::string{} : std::string(v);
}

// An optional positive id from the query string; nullopt if it's malformed.
std::optional<int64_t> queryId(uWS::HttpRequest* req, std::string_view key) {
    auto v = query(req, key);
    if (v.empty()) return 0;
    auto id = parseId(v);
    if (!id || *id <= 0) return std::nullopt;
    return id;
}

// 1..500, default 100.
std::optional<int> queryLimit(uWS::HttpRequest* req) {
    auto v = query(req, "limit");
    if (v.empty()) return 100;
    auto n = parseId(v);
    if (!n || *n < 1) return std::nullopt;
    return static_cast<int>(std::min<int64_t>(*n, 500));
}

template <bool SSL>
void respond(uWS::HttpResponse<SSL>* res, const json& body) {
    res->writeHeader("Content-Type", "application/json")->end(body.dump());
}

template <bool SSL>
void dbError(uWS::HttpResponse<SSL>* res, const DbOutcome& outcome) {
    std::cerr << "Audit query error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
    res->writeStatus("500 Internal Server Error")->end("Server error");
}

// The page's oldest id, for ?before= on the next request; null at the end.
json nextBefore(const json& rows, int limit) {
    if (static_cast<int>(rows.size()) < limit || rows.empty()) return nullptr;
    return rows.back()["id"];
}

} // namespace

template <bool SSL>
void registerAuditRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, AuditLog& audit) {

app.get("/api/audit/events", [&auth, &db, &audit](auto* res, auto* req) {
    if (!requireRole(auth, db, res, req, Role::Owner)) return;
    AuditLog::EventFilter f;
    f.type  = query(req, "type");
    f.since = query(req, "since");
    f.until = query(req, "until");
    auto user = queryId(req, "user_id"), target = queryId(req, "target_id"),
         before = queryId(req, "before");
    auto limit = queryLimit(req);
    if (!user || !target || !before || !limit || f.type.size() > 64 || f.since.size() > 32 ||
        f.until.size() > 32) {
        res->writeStatus("400 Bad Request")->end("Invalid query");
        return;
    }
    f.userId = *user; f.targetId = *target; f.beforeId = *before; f.limit = *limit;
    DbOutcome outcome;
    auto rows = audit.events(f, outcome);
    if (!rows) { dbError(res, outcome); return; }
    respond(res, json{{"events", *rows}, {"next_before", nextBefore(*rows, f.limit)}});
})

.get("/api/audit/control-sessions", [&auth, &db, &audit](auto* res, auto* req) {
    if (!requireRole(auth, db, res, req, Role::Owner)) return;
    auto user = queryId(req, "user_id"), target = queryId(req, "target_id"),
         before = queryId(req, "before");
    auto limit = queryLimit(req);
    if (!user || !target || !before || !limit) {
        res->writeStatus("400 Bad Request")->end("Invalid query");
        return;
    }
    AuditLog::SessionFilter f{*user, *target, *before, *limit};
    DbOutcome outcome;
    auto rows = audit.controlSessions(f, outcome);
    if (!rows) { dbError(res, outcome); return; }
    respond(res, json{{"sessions", *rows}, {"next_before", nextBefore(*rows, f.limit)}});
});

}

// Plain HTTP and HTTPS run the same routes.
template void registerAuditRoutes<false>(uWS::App&, Auth&, Database&, AuditLog&);
template void registerAuditRoutes<true>(uWS::SSLApp&, Auth&, Database&, AuditLog&);

} // namespace houston_kvm
