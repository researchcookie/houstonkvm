#include "video/routes_settings.h"

#include "core/http_common.h"
#include "core/routes_targets.h"
#include "core/target_http.h"
#include "hid/serial_ports.h"
#include "video/audio_capture.h"
#include "video/v4l2_capture.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace houston_kvm {

using json = nlohmann::json;

namespace {

// Guards the ?device= override on /api/video/capabilities, which — unlike
// the saved v4l2_device string PUT accepts — is used to open() a
// caller-supplied path immediately, server-side. Restricting it to the
// actual /dev/video* node shape (rather than trusting Operator+ the same
// way the PUT does) keeps that open() from being a free-form local path
// probe.
bool isValidVideoDevicePath(const std::string& s) {
    static constexpr std::string_view prefix = "/dev/video";
    if (s.size() <= prefix.size() || s.compare(0, prefix.size(), prefix) != 0) return false;
    return std::all_of(s.begin() + prefix.size(), s.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

} // namespace

template <bool SSL>
void registerSettingsRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                            EventBus& events) {

// ── Per-target capture settings (Operator+) ──────────────────────────────
//
// Each has an unscoped form (default target) and a /api/targets/:id/...
// form. Operators may retune resolution/fps/bitrate; the hardware-wiring
// fields stay Owner-only (PUT /api/targets/:id, or /api/admin/settings below).

auto videoSettingsGet = [&auth, &db, &targets](bool scoped) {
    return [&auth, &db, &targets, scoped](auto* res, auto* req) {
        auto authOpt = requireBrowserRole(auth, db, res, req, Role::Operator);
        if (!authOpt) return;
        auto target = resolveTargetRow(targets, db, res, req, scoped);
        if (!target) return;

        const auto& s = target->settings;
        // active_* reports what the running capture actually negotiated (see
        // V4l2Capture::width()/height()/fps()) rather than the saved request —
        // the driver is free to round or reject the saved values, and the UI
        // needs to tell the two apart. null when no capture is running
        // (e.g. device missing, or the target is disabled) so the UI can show
        // "stream not running".
        json active = nullptr;
        if (auto* runtime = targets.find(target->id)) {
            runtime->streamManager.withCapture([&active](CaptureSource* cap) {
                if (cap) active = json{{"width", cap->width()},
                                       {"height", cap->height()},
                                       {"fps", cap->fps()}};
            });
        }
        res->writeHeader("Content-Type", "application/json")
           ->end(json{{"capture_width", s.captureWidth},
                      {"capture_height", s.captureHeight},
                      {"capture_fps", s.captureFps},
                      {"webrtc_bitrate_kbps", s.webrtcBitrateKbps},
                      {"active", active}}.dump());
    };
};

// Drives the settings UI's resolution/fps picker: reports only the modes
// the capture hardware itself advertises (VIDIOC_ENUM_FRAMESIZES /
// FRAMEINTERVALS), same idea as how a display picker only ever offers
// modes the monitor's EDID reports. Defaults to the target's configured
// device (even if no capture is running right now); the Admin page's
// device picker passes ?device=/dev/videoN to preview a not-yet-saved
// device's modes before committing to it.
auto videoCapabilities = [&auth, &db, &targets](bool scoped) {
    return [&auth, &db, &targets, scoped](auto* res, auto* req) {
        auto authOpt = requireBrowserRole(auth, db, res, req, Role::Operator);
        if (!authOpt) return;
        std::string device;
        if (auto q = req->getQuery("device"); !q.empty()) {
            device = std::string(q);
            if (!isValidVideoDevicePath(device)) {
                res->writeStatus("400 Bad Request")->end("Invalid device");
                return;
            }
            // Probing a device the caller named needs no target — and must
            // work with none, since that's exactly when someone is adding
            // the first one. The scoped form still insists the id exists.
            if (scoped && !resolveTargetRow(targets, db, res, req, true)) return;
        } else {
            auto target = resolveTargetRow(targets, db, res, req, scoped);
            if (!target) return;
            device = target->settings.v4l2Device;
        }

        json modes = json::array();
        std::string error;
        try {
            for (const auto& m : V4l2Capture::enumerateModes(device))
                modes.push_back({{"width", m.width}, {"height", m.height}, {"fps", m.fps}});
        } catch (const V4l2Exception& e) {
            error = e.what();
        }
        res->writeHeader("Content-Type", "application/json")
           ->end(json{{"modes", modes}, {"error", error}}.dump());
    };
};

auto videoSettingsPut = [&auth, &db, &targets, &events](bool scoped) {
    return [&auth, &db, &targets, &events, scoped](auto* res, auto* req) {
        auto authOpt = requireBrowserRole(auth, db, res, req, Role::Operator);
        if (!authOpt) return;
        auto id = resolveTargetId(targets, res, req, scoped); // before the body arrives
        if (!id) return;
        Actor actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));

        readBody(res, 1024, [res, targetId = *id, &db, &targets, &events, actor](std::string& body) {
            auto j = parseJsonOr400(res, body);
            if (!j) return;
            auto target = db.getTarget(targetId);
            if (!target) { res->writeStatus("404 Not Found")->end("Unknown target"); return; }
            const Database::Target before = *target;

            // Only the four operator-level keys are honoured; anything else
            // in the body (device paths, serial config) is ignored here, not
            // applied — those need Owner.
            json allowed = json::object();
            for (const char* key : {"capture_width", "capture_height", "capture_fps", "webrtc_bitrate_kbps"})
                if (j->is_object() && j->contains(key)) allowed[key] = (*j)[key];
            if (auto err = applySettingsFields(allowed, target->settings); !err.empty()) {
                res->writeStatus("400 Bad Request")->end(err);
                return;
            }
            if (!db.updateTarget(*target)) {
                res->writeStatus("500 Internal Server Error")->end("Server error");
                return;
            }
            // Rebuild happens on the target's own StreamManager worker
            // thread — this returns immediately, same as PUT /api/targets/:id.
            if (auto fresh = db.getTarget(targetId)) {
                if (auto changes = auditChanges(before, *fresh); !changes.empty())
                    emitTarget(events, event::kVideoSettings, actor, *fresh, {{"changes", changes}});
                targets.apply(*fresh);
            }

            res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
        });
    };
};


app.get("/api/settings", [&auth, &db](auto* res, auto* req) {
    auto uidOpt = requireAuth(auth, db, res, req);
    if (!uidOpt) return;

    Stmt s(db.handle(),
        "SELECT theme, default_stream_mode, webrtc_bitrate_kbps, "
        "       webrtc_bitrate_kbps IS NULL "
        "FROM user_settings WHERE user_id = ?");
    s.bind(1, static_cast<sqlite3_int64>(*uidOpt));
    auto step = s.step();
    if (step == Stmt::StepResult::Error) {
        std::cerr << "Get settings error [sqlite:" << s.outcome().sqliteCode << "]: "
                   << s.outcome().message << "\n";
        res->writeStatus("500 Internal Server Error")->end("Server error");
        return;
    }
    std::string theme = "dark", streamMode = "mjpeg";
    json bitrate = nullptr;
    if (step == Stmt::StepResult::Row) {
        theme      = s.column_text(0);
        streamMode = s.column_text(1);
        if (s.column_int(3) == 0) bitrate = s.column_int(2);
    }
    res->writeHeader("Content-Type", "application/json")
       ->end(json{{"theme", theme},
                  {"default_stream_mode", streamMode},
                  {"webrtc_bitrate_kbps", bitrate}}.dump());
})

.put("/api/settings", [&auth, &db](auto* res, auto* req) {
    auto uidOpt = requireBrowserAuth(auth, db, res, req);
    if (!uidOpt) return;
    int64_t uid = *uidOpt;

    auto body = std::make_shared<std::string>();
    res->onData([res, body, &db, uid](std::string_view chunk, bool last) mutable {
        if (body->size() + chunk.size() > 4096) {
            res->writeStatus("413 Payload Too Large")->end();
            return;
        }
        body->append(chunk);
        if (!last) return;

        auto j = json::parse(*body, nullptr, false);
        if (j.is_discarded()) {
            res->writeStatus("400 Bad Request")->end("Invalid JSON");
            return;
        }
        auto themeOpt      = jsonValueOr(j, "theme", std::string{"dark"});
        auto streamModeOpt = jsonValueOr(j, "default_stream_mode", std::string{"mjpeg"});
        if (!themeOpt || !streamModeOpt) {
            res->writeStatus("400 Bad Request")->end("Invalid field types");
            return;
        }
        const std::string& theme      = *themeOpt;
        const std::string& streamMode = *streamModeOpt;
        if (theme != "dark" && theme != "light") {
            res->writeStatus("400 Bad Request")->end("Invalid theme");
            return;
        }
        if (streamMode != "mjpeg" && streamMode != "webrtc") {
            res->writeStatus("400 Bad Request")->end("Invalid default_stream_mode");
            return;
        }

        std::optional<int64_t> bitrate;
        if (auto it = j.find("webrtc_bitrate_kbps"); it != j.end() && !it->is_null()) {
            if (!it->is_number_integer()) {
                res->writeStatus("400 Bad Request")->end("Invalid field types");
                return;
            }
            bitrate = it->get<int64_t>();
        }

        Stmt s(db.handle(), R"(
            INSERT INTO user_settings (user_id, theme, default_stream_mode, webrtc_bitrate_kbps, updated_at)
            VALUES (?, ?, ?, ?, datetime('now'))
            ON CONFLICT(user_id) DO UPDATE SET
                theme = excluded.theme,
                default_stream_mode = excluded.default_stream_mode,
                webrtc_bitrate_kbps = excluded.webrtc_bitrate_kbps,
                updated_at = excluded.updated_at
        )");
        s.bind(1, static_cast<sqlite3_int64>(uid))
         .bind(2, theme)
         .bind(3, streamMode);
        if (bitrate) s.bind(4, static_cast<sqlite3_int64>(*bitrate));
        else         s.bindNull(4);

        if (s.step() == Stmt::StepResult::Error) {
            std::cerr << "Save settings error [sqlite:" << s.outcome().sqliteCode << "]: "
                       << s.outcome().message << "\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
    });
    res->onAborted([](){});
})
.get("/api/video/settings", videoSettingsGet(false))
   .get("/api/targets/:id/video/settings", videoSettingsGet(true))
   .put("/api/video/settings", videoSettingsPut(false))
   .put("/api/targets/:id/video/settings", videoSettingsPut(true))
   .get("/api/video/capabilities", videoCapabilities(false))
   .get("/api/targets/:id/video/capabilities", videoCapabilities(true))

// Legacy full hardware-config route from before there were multiple
// targets: reads/writes the *default* target's wiring. New clients should
// use GET/PUT /api/targets/:id instead, which also covers name/group/tags.
.get("/api/admin/settings", [&auth, &db, &targets](auto* res, auto* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;
    auto target = resolveTargetRow(targets, db, res, req, false);
    if (!target) return;

    res->writeHeader("Content-Type", "application/json")
       ->end(targetSettingsToJson(target->settings).dump());
})

// Drives the Admin page's capture-device and serial-device pickers: what's
// actually plugged in right now, rather than requiring the Owner to already
// know a /dev/video* or /dev/ttyUSB* path — the whole point being to make
// first-time setup not depend on knowing Linux device-node conventions.
// in_use_by names the target already wired to a device (or null), so with
// many dongles and serial adapters attached it's obvious which are free.
.get("/api/admin/devices", [&auth, &db](auto* res, auto* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;

    const auto allTargets = db.listTargets();
    auto usedBy = [&allTargets](auto matches) -> json {
        for (const auto& t : allTargets)
            if (matches(t.settings)) return json{{"id", t.id}, {"name", t.name}};
        return nullptr;
    };

    json videoDevices = json::array();
    for (const auto& d : V4l2Capture::enumerateDevices())
        videoDevices.push_back({{"path", d.path}, {"name", d.name}, {"mjpeg", d.mjpegCapable},
                                {"in_use_by", usedBy([&d](const Database::TargetSettings& s) {
                                    return s.v4l2Device == d.path; })}});

    json serialDevices = json::array();
    for (const auto& p : enumerateSerialPorts())
        serialDevices.push_back({{"path", p.path}, {"label", p.label},
                                 {"in_use_by", usedBy([&p](const Database::TargetSettings& s) {
                                     return s.serialDevice == p.path; })}});

    // A target set to "auto" uses its capture dongle's own sound card, so
    // that card counts as in use too. Resolved once per target: it's a sysfs scan.
    std::map<std::string, std::string> autoAudio; // v4l2 device -> its sound card
    for (const auto& t : allTargets)
        if (t.settings.audioDevice == "auto")
            autoAudio.emplace(t.settings.v4l2Device, audioDeviceFor(t.settings.v4l2Device));
    json audioDevices = json::array();
    for (const auto& d : enumerateAudioDevices())
        audioDevices.push_back({{"device", d.device}, {"name", d.name},
                                {"in_use_by", usedBy([&d, &autoAudio](const Database::TargetSettings& s) {
                                    return s.audioDevice == d.device ||
                                           (s.audioDevice == "auto" && autoAudio[s.v4l2Device] == d.device);
                                })}});

    res->writeHeader("Content-Type", "application/json")
       ->end(json{{"video_devices", videoDevices}, {"serial_devices", serialDevices},
                  {"audio_devices", audioDevices}, {"audio_supported", audioSupported()}}.dump());
})

.put("/api/admin/settings", [&auth, &db, &targets, &events](auto* res, auto* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;
    auto id = resolveTargetId(targets, res, req, false); // before the body arrives
    if (!id) return;
    Actor actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));

    readBody(res, 4096, [res, targetId = *id, &db, &targets, &events, actor](std::string& body) {
        auto j = parseJsonOr400(res, body);
        if (!j) return;
        auto target = db.getTarget(targetId);
        if (!target) { res->writeStatus("404 Not Found")->end("Unknown target"); return; }
        const Database::Target before = *target;

        // Full replace, as this route always was: fields absent from the
        // body fall back to defaults rather than keeping their current value.
        Database::TargetSettings s;
        if (auto err = applySettingsFields(*j, s); !err.empty()) {
            res->writeStatus("400 Bad Request")->end(err);
            return;
        }
        target->settings = s;

        DbOutcome outcome;
        if (!db.updateTarget(*target, &outcome)) {
            if (auto conflict = describeTargetConflict(outcome); !conflict.empty())
                res->writeStatus("409 Conflict")->end(conflict);
            else
                res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        // Rebuild happens on the target's own StreamManager worker thread —
        // this returns immediately. Snapshot/stream/webrtc-subscribe may 503
        // briefly (up to ~11s) while capture/publisher/input are rebuilt.
        if (auto fresh = db.getTarget(targetId)) {
            if (auto changes = auditChanges(before, *fresh); !changes.empty())
                emitTarget(events, event::kTargetUpdated, actor, *fresh, {{"changes", changes}});
            targets.apply(*fresh);
        }

        res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
    });
});

}

// Plain HTTP and HTTPS run the same routes.
template void registerSettingsRoutes<false>(uWS::App&, Auth&, Database&, TargetManager&, EventBus&);
template void registerSettingsRoutes<true>(uWS::SSLApp&, Auth&, Database&, TargetManager&, EventBus&);

} // namespace houston_kvm
