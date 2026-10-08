#pragma once

#include "core/http_common.h"
#include "core/target_manager.h"

#include <optional>

namespace houston_kvm {

// Which target does this request address? Routes come in two forms —
// /api/targets/:id/... (scoped: :id is parameter 0) and the original
// unscoped /api/... paths, which address the explicitly-designated default
// target. Writes the error response and returns nullopt on failure.
//
// Must be called synchronously from the route handler (it reads
// req->getParameter(), which is invalid once the handler returns), and only
// after authentication, so an unauthenticated caller learns nothing about
// which target ids exist.
template <bool SSL>
inline std::optional<int64_t> resolveTargetId(TargetManager& targets,
                                               uWS::HttpResponse<SSL>* res,
                                               uWS::HttpRequest* req, bool scoped) {
    if (scoped) {
        auto id = parseId(req->getParameter(0));
        if (!id) {
            res->writeStatus("400 Bad Request")->end("Invalid target id");
            return std::nullopt;
        }
        return id;
    }
    int64_t id = targets.defaultId();
    if (id == 0) {
        res->writeStatus("404 Not Found")->end("No default target configured");
        return std::nullopt;
    }
    return id;
}

// Resolves to a *running* target (see TargetManager::find for the lifetime
// rules on the returned pointer). A target that exists but isn't running is
// reported as 503 rather than 404, so a caller can tell "wrong id" from
// "disabled or still starting".
template <bool SSL>
inline TargetRuntime* resolveRunningTarget(TargetManager& targets, Database& db,
                                            uWS::HttpResponse<SSL>* res,
                                            uWS::HttpRequest* req, bool scoped) {
    auto id = resolveTargetId(targets, res, req, scoped);
    if (!id) return nullptr;
    if (auto* runtime = targets.find(*id)) return runtime;
    if (targets.isStarting(*id)) {
        res->writeStatus("503 Service Unavailable")->end("Target is starting");
    } else if (db.getTarget(*id)) {
        res->writeStatus("503 Service Unavailable")->end("Target is disabled");
    } else {
        res->writeStatus("404 Not Found")->end("Unknown target");
    }
    return nullptr;
}

// Resolves to a target's database row, running or not — for settings routes,
// which must work on a disabled target too.
template <bool SSL>
inline std::optional<Database::Target> resolveTargetRow(TargetManager& targets, Database& db,
                                                         uWS::HttpResponse<SSL>* res,
                                                         uWS::HttpRequest* req, bool scoped) {
    auto id = resolveTargetId(targets, res, req, scoped);
    if (!id) return std::nullopt;
    auto target = db.getTarget(*id);
    if (!target) res->writeStatus("404 Not Found")->end("Unknown target");
    return target;
}

} // namespace houston_kvm
