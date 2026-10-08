#include "core/routes_targets.h"

#include "core/http_common.h"
#include "core/target_http.h"
#include "video/audio_capture.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace houston_kvm {

namespace {

constexpr size_t kMaxNameLen        = 64;
constexpr size_t kMaxDescriptionLen = 512;
constexpr size_t kMaxGroupLen       = 64;
constexpr size_t kMaxTags           = 16;
constexpr size_t kMaxTagLen         = 32;

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool hasControlChars(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

// getQuery() returns a view over nothing for an absent key; normalize that
// to an empty std::string rather than constructing one from a null view.
std::string queryParam(uWS::HttpRequest* req, std::string_view key) {
    auto v = req->getQuery(key);
    return v.empty() ? std::string{} : std::string(v);
}

// Tags are lowercase [a-z0-9._:/-], sorted, deduplicated. Normalizing at
// the door is what keeps "Rack-3" and "rack-3" from becoming two tags.
std::string parseTags(const json& value, std::vector<std::string>& out) {
    if (!value.is_array()) return "Invalid field types";
    std::set<std::string> unique;
    for (const auto& item : value) {
        if (!item.is_string()) return "Invalid field types";
        std::string tag = lower(trim(item.get<std::string>()));
        if (tag.empty()) continue;
        if (tag.size() > kMaxTagLen) return "Tag too long (max 32 characters)";
        bool valid = std::isalnum(static_cast<unsigned char>(tag[0])) &&
            std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '.' || c == '_' || c == ':' || c == '/' || c == '-';
            });
        if (!valid) return "Invalid tag (use letters, digits and . _ : / -)";
        unique.insert(std::move(tag));
    }
    if (unique.size() > kMaxTags) return "Too many tags (max 16)";
    out.assign(unique.begin(), unique.end());
    return "";
}

// Applies the fields present in `j` onto `t` — name/description/group/tags/
// enabled plus the hardware fields. Sets makeDefault if "default": true.
std::string applyTargetFields(const json& j, Database::Target& t, bool& makeDefault) {
    auto name        = jsonValueOr(j, "name", t.name);
    auto description = jsonValueOr(j, "description", t.description);
    auto group       = jsonValueOr(j, "group", t.group);
    auto enabled     = jsonValueOr(j, "enabled", t.enabled);
    if (!name || !description || !group || !enabled) return "Invalid field types";

    t.name        = trim(*name);
    t.description = trim(*description);
    t.group       = trim(*group);
    t.enabled     = *enabled;

    if (t.name.empty() || t.name.size() > kMaxNameLen || hasControlChars(t.name))
        return "Invalid name (1-64 characters)";
    if (t.description.size() > kMaxDescriptionLen || hasControlChars(t.description))
        return "Invalid description (max 512 characters)";
    if (t.group.size() > kMaxGroupLen || hasControlChars(t.group))
        return "Invalid group (max 64 characters)";

    if (auto it = j.find("tags"); it != j.end()) {
        std::string err = parseTags(*it, t.tags);
        if (!err.empty()) return err;
    }

    if (auto it = j.find("default"); it != j.end()) {
        if (!it->is_boolean()) return "Invalid field types";
        // There is deliberately no way to un-default a target: with no
        // default, unscoped routes fail, so the only sane transition is to
        // designate a different one.
        if (!it->get<bool>()) return "default can only be set to true; designate a different default instead";
        makeDefault = true;
    }

    return applySettingsFields(j, t.settings);
}

json targetToJson(const Database::Target& t, TargetManager& targets, bool includeConfig) {
    json j{
        {"id", t.id},
        {"name", t.name},
        {"description", t.description},
        {"group", t.group},
        {"tags", t.tags},
        {"enabled", t.enabled},
        {"default", t.isDefault},
    };

    // "disabled": nothing running by choice. "starting": enabled but its
    // runtime isn't up yet (waiting on a previous run to release its
    // devices). "running": pipeline built — whether capture/input actually
    // came up is in `status`, since a missing dongle isn't fatal.
    auto* runtime = targets.find(t.id);
    j["state"] = !t.enabled ? "disabled" : runtime ? "running" : "starting";

    json status{{"capture_active", false}, {"video_state", "starting"}, {"webrtc_viewers", 0},
                {"input_ready", false}, {"audio_state", "off"},
                {"input_health", "unknown"}, {"controlled", false}, {"driver", ""}};
    if (runtime) {
        status["capture_active"] = runtime->streamManager.withCapture(
            [](CaptureSource* cap) { return cap && cap->isRunning(); });
        status["video_state"] = videoStateName(runtime->streamManager.videoHealth().state);
        status["webrtc_viewers"] = runtime->streamManager.webrtcViewers();
        status["audio_state"] = audioStateName(runtime->streamManager.audioHealth().state);
        status["input_ready"] = runtime->streamManager.withInput(
            [](InputBackend* b) { return b && b->isReady(); });
        status["input_health"] = runtime->streamManager.withInput([](InputBackend* b) {
            return healthStateName(b ? b->health().state : HealthState::Unknown);
        });
        status["controlled"] = !runtime->driverToken.empty();
        status["driver"] = runtime->driverName;
    }
    j["status"] = std::move(status);

    if (includeConfig) j["config"] = targetSettingsToJson(t.settings);
    return j;
}

template <bool SSL>
void respondJson(uWS::HttpResponse<SSL>* res, const char* status, const json& body) {
    res->writeStatus(status)->writeHeader("Content-Type", "application/json")->end(body.dump());
}

// Device paths are only for an Owner's browser session (same bar as
// /api/admin/settings) — not Viewers/Operators, not API tokens.
bool mayViewConfig(const AuthResult& auth, uWS::HttpRequest* req) {
    return auth.role >= Role::Owner && req->getHeader("authorization").empty();
}

} // namespace

std::string applySettingsFields(const json& j, Database::TargetSettings& s) {
    auto device  = jsonValueOr(j, "v4l2_device", s.v4l2Device);
    auto width   = jsonValueOr(j, "capture_width", static_cast<int>(s.captureWidth));
    auto height  = jsonValueOr(j, "capture_height", static_cast<int>(s.captureHeight));
    auto fps     = jsonValueOr(j, "capture_fps", static_cast<int>(s.captureFps));
    auto qmp     = jsonValueOr(j, "qmp_socket", s.qmpSocket);
    auto serial  = jsonValueOr(j, "serial_device", s.serialDevice);
    auto baud    = jsonValueOr(j, "serial_baud", s.serialBaud);
    auto bitrate = jsonValueOr(j, "webrtc_bitrate_kbps", static_cast<int>(s.webrtcBitrateKbps));
    auto audio   = jsonValueOr(j, "audio_device", s.audioDevice);
    if (!device || !width || !height || !fps || !qmp || !serial || !baud || !bitrate || !audio)
        return "Invalid field types";

    if (device->empty() || device->size() > 256) return "Invalid v4l2_device";
    if (*width <= 0 || *width > 7680 || *height <= 0 || *height > 4320)
        return "Invalid capture_width/capture_height";
    if (*fps <= 0 || *fps > 120) return "Invalid capture_fps";
    if (qmp->size() > 256 || serial->size() > 256) return "qmp_socket/serial_device too long";
    if (*baud <= 0) return "Invalid serial_baud";
    if (*bitrate <= 0 || *bitrate > 100000) return "Invalid webrtc_bitrate_kbps";
    if (!isValidAudioSetting(*audio))
        return "Invalid audio_device: use \"auto\", \"\" for none, or an ALSA hw:, plughw: or "
               "sysdefault: device";

    s.v4l2Device        = *device;
    s.captureWidth      = static_cast<uint32_t>(*width);
    s.captureHeight     = static_cast<uint32_t>(*height);
    s.captureFps        = static_cast<uint32_t>(*fps);
    s.qmpSocket         = *qmp;
    s.serialDevice      = *serial;
    s.serialBaud        = *baud;
    s.webrtcBitrateKbps = static_cast<uint32_t>(*bitrate);
    s.audioDevice       = *audio;
    return "";
}

json targetSettingsToJson(const Database::TargetSettings& s) {
    return json{
        {"v4l2_device", s.v4l2Device},
        {"capture_width", s.captureWidth},
        {"capture_height", s.captureHeight},
        {"capture_fps", s.captureFps},
        {"qmp_socket", s.qmpSocket},
        {"serial_device", s.serialDevice},
        {"serial_baud", s.serialBaud},
        {"webrtc_bitrate_kbps", s.webrtcBitrateKbps},
        {"audio_device", s.audioDevice},
    };
}

std::string describeTargetConflict(const DbOutcome& outcome) {
    if (!outcome.isConstraintViolation()) return "";
    const std::string& m = outcome.message;
    auto has = [&m](const char* needle) { return m.find(needle) != std::string::npos; };
    if (has("targets.name"))
        return "A target with that name already exists";
    if (has("idx_targets_v4l2") || has("targets.v4l2_device"))
        return "That capture device is already used by another target";
    if (has("idx_targets_serial") || has("targets.serial_device"))
        return "That serial device is already used by another target";
    if (has("idx_targets_qmp") || has("targets.qmp_socket"))
        return "That QEMU socket is already used by another target";
    if (has("idx_targets_audio") || has("targets.audio_device"))
        return "That sound device is already used by another target";
    if (has("idx_targets_gadget"))
        return "Only one target can use the local USB HID gadget (no serial device or QEMU "
               "socket set) — give this one a serial device or QEMU socket";
    return "Conflicts with another target";
}

// What the audit log keeps about a target: everything an Owner can set.
json auditView(const Database::Target& t) {
    json j{{"name", t.name}, {"description", t.description}, {"group", t.group},
           {"tags", t.tags}, {"enabled", t.enabled}, {"default", t.isDefault}};
    json settings = targetSettingsToJson(t.settings);   // named: items() mustn't outlive it
    for (auto& [k, v] : settings.items()) j[k] = v;
    return j;
}

// {"field": {"from": old, "to": new}} for each field an update changed.
json auditChanges(const Database::Target& before, const Database::Target& after) {
    json a = auditView(before), b = auditView(after), changes = json::object();
    for (auto& [k, v] : b.items())
        if (a[k] != v) changes[k] = {{"from", a[k]}, {"to", v}};
    return changes;
}

void emitTarget(EventBus& events, std::string_view type, const Actor& actor,
                const Database::Target& t, json detail) {
    Event e{type, event::kOk, actor};
    e.targetId = t.id;
    e.targetName = t.name;
    e.detail = std::move(detail);
    events.emit(e);
}

template <bool SSL>
void registerTargetRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                          EventBus& events) {

app.get("/api/targets", [&auth, &db, &targets](auto* res, auto* req) {
    auto authOpt = requireRole(auth, db, res, req, Role::Viewer);
    if (!authOpt) return;

    Database::TargetQuery query;
    query.text        = queryParam(req, "q");
    query.tag         = lower(queryParam(req, "tag"));
    query.group       = queryParam(req, "group");
    query.enabledOnly = queryParam(req, "enabled") == "1";
    const bool config = mayViewConfig(*authOpt, req);

    json list = json::array();
    for (const auto& t : db.listTargets(query)) list.push_back(targetToJson(t, targets, config));
    respondJson(res, "200 OK", json{{"targets", list}});
})

// Registered alongside /api/targets/:id — uWS tries static path segments
// before parameters, so "facets" is never mistaken for an id.
.get("/api/targets/facets", [&auth, &db](auto* res, auto* req) {
    if (!requireRole(auth, db, res, req, Role::Viewer)) return;

    json groups = json::array(), tags = json::array();
    for (const auto& g : db.listGroups()) groups.push_back({{"name", g.value}, {"count", g.count}});
    for (const auto& t : db.listTags())   tags.push_back({{"name", t.value}, {"count", t.count}});
    respondJson(res, "200 OK", json{{"groups", groups}, {"tags", tags}});
})

.get("/api/targets/:id", [&auth, &db, &targets](auto* res, auto* req) {
    auto authOpt = requireRole(auth, db, res, req, Role::Viewer);
    if (!authOpt) return;
    auto id = parseId(req->getParameter(0));
    if (!id) { res->writeStatus("400 Bad Request")->end("Invalid target id"); return; }
    auto target = db.getTarget(*id);
    if (!target) { res->writeStatus("404 Not Found")->end("Unknown target"); return; }
    respondJson(res, "200 OK", targetToJson(*target, targets, mayViewConfig(*authOpt, req)));
})

.post("/api/targets", [&auth, &db, &targets, &events](auto* res, auto* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;
    Actor actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));

    readBody(res, 4096, [res, &db, &targets, &events, actor](std::string& body) {
        auto j = parseJsonOr400(res, body);
        if (!j) return;

        Database::Target target;
        bool makeDefault = false;
        if (auto err = applyTargetFields(*j, target, makeDefault); !err.empty()) {
            res->writeStatus("400 Bad Request")->end(err);
            return;
        }

        DbOutcome outcome;
        int64_t id = db.createTarget(target, &outcome);
        if (id == 0) {
            if (auto conflict = describeTargetConflict(outcome); !conflict.empty())
                res->writeStatus("409 Conflict")->end(conflict);
            else
                res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        if (makeDefault) {
            if (auto fresh = db.getTarget(id); fresh && !fresh->isDefault) {
                fresh->isDefault = true;
                db.updateTarget(*fresh);
            }
        }
        auto created = db.getTarget(id);
        if (!created) { res->writeStatus("500 Internal Server Error")->end("Server error"); return; }
        emitTarget(events, event::kTargetCreated, actor, *created, auditView(*created));
        targets.apply(*created);
        respondJson(res, "201 Created", targetToJson(*created, targets, true));
    });
})

.put("/api/targets/:id", [&auth, &db, &targets, &events](auto* res, auto* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;
    auto id = parseId(req->getParameter(0)); // must be read before the body arrives
    if (!id) { res->writeStatus("400 Bad Request")->end("Invalid target id"); return; }
    const int64_t targetId = *id;
    Actor actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));

    readBody(res, 4096, [res, targetId, &db, &targets, &events, actor](std::string& body) {
        auto j = parseJsonOr400(res, body);
        if (!j) return;

        auto target = db.getTarget(targetId);
        if (!target) { res->writeStatus("404 Not Found")->end("Unknown target"); return; }
        const Database::Target before = *target;

        bool makeDefault = false;
        if (auto err = applyTargetFields(*j, *target, makeDefault); !err.empty()) {
            res->writeStatus("400 Bad Request")->end(err);
            return;
        }
        target->isDefault = makeDefault;

        DbOutcome outcome;
        if (!db.updateTarget(*target, &outcome)) {
            if (auto conflict = describeTargetConflict(outcome); !conflict.empty())
                res->writeStatus("409 Conflict")->end(conflict);
            else
                res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        auto updated = db.getTarget(targetId);
        if (!updated) { res->writeStatus("500 Internal Server Error")->end("Server error"); return; }
        if (auto changes = auditChanges(before, *updated); !changes.empty())
            emitTarget(events, event::kTargetUpdated, actor, *updated, {{"changes", changes}});
        // Rebuilds the pipeline only if the hardware settings changed;
        // renaming/retagging leaves a running target untouched.
        targets.apply(*updated);
        respondJson(res, "200 OK", targetToJson(*updated, targets, true));
    });
})

.del("/api/targets/:id", [&auth, &db, &targets, &events](auto* res, auto* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;
    auto id = parseId(req->getParameter(0));
    if (!id) { res->writeStatus("400 Bad Request")->end("Invalid target id"); return; }
    auto doomed = db.getTarget(*id);
    if (!doomed) { res->writeStatus("404 Not Found")->end("Unknown target"); return; }

    if (!db.deleteTarget(*id)) {
        res->writeStatus("500 Internal Server Error")->end("Server error");
        return;
    }
    emitTarget(events, event::kTargetDeleted, actorOf(*authOpt, clientAddress(auth.policy, res, req)),
               *doomed, auditView(*doomed));
    // Stops capture/encode/input and drops any driver lock; if this was the
    // default, unscoped routes now fail until another is designated.
    targets.remove(*id);
    respondJson(res, "200 OK", json{{"ok", true}});
});

}

// Plain HTTP and HTTPS run the same routes.
template void registerTargetRoutes<false>(uWS::App&, Auth&, Database&, TargetManager&, EventBus&);
template void registerTargetRoutes<true>(uWS::SSLApp&, Auth&, Database&, TargetManager&, EventBus&);

} // namespace houston_kvm
