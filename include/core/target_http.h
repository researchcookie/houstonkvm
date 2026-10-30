#pragma once

#include "core/http_common.h"
#include "core/routes_targets.h"
#include "core/target_manager.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

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

// The rest of an Operator's change to a few of a target's fields, once the
// route has checked who is asking and which target: reads the JSON body, has
// `applyFields` put the fields it honours on the row (a non-empty error is a
// 400), saves it, audits what changed as `eventType` and applies it to the
// running target. Answers {"ok":true} at once: anything slow (a rebuild)
// happens on the target's own threads.
template <bool SSL>
inline void updateTargetFields(uWS::HttpResponse<SSL>* res, int64_t targetId, Database& db,
                               TargetManager& targets, EventBus& events, Actor actor,
                               std::string_view eventType,
                               std::function<std::string(const json&, Database::Target&)> applyFields) {
    readBody(res, 1024, [res, targetId, &db, &targets, &events, actor = std::move(actor), eventType,
                         applyFields = std::move(applyFields)](std::string& body) {
        auto j = parseJsonOr400(res, body);
        if (!j) return;
        auto target = db.getTarget(targetId);
        if (!target) { res->writeStatus("404 Not Found")->end("Unknown target"); return; }
        const Database::Target before = *target;
        if (auto err = applyFields(*j, *target); !err.empty()) {
            res->writeStatus("400 Bad Request")->end(err);
            return;
        }
        if (!db.updateTarget(*target)) {
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        if (auto fresh = db.getTarget(targetId)) {
            if (auto changes = auditChanges(before, *fresh); !changes.empty())
                emitTarget(events, eventType, actor, *fresh, {{"changes", changes}});
            targets.apply(*fresh);
        }
        res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
    });
}

} // namespace houston_kvm
