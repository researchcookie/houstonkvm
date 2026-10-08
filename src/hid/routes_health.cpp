#include "hid/routes_health.h"

#include "core/http_common.h"
#include "core/target_http.h"
#include "hid/input_backend.h"
#include "video/audio_capture.h"

#include <algorithm>
#include <chrono>

namespace houston_kvm {

using json = nlohmann::json;

namespace {

template <typename T>
json optionalJson(const std::optional<T>& v) {
    return v ? json(*v) : json(nullptr);
}

json firmwareJson(const std::optional<int>& v) {
    if (!v) return nullptr;
    return std::to_string(*v >> 4) + "." + std::to_string(*v & 0x0F);
}

json selfTestToJson(const InputSelfTest& t, bool supported) {
    if (!supported) return nullptr;
    const char* phase = t.phase == InputSelfTest::Phase::Running ? "running"
                      : t.phase == InputSelfTest::Phase::Done    ? "done" : "idle";
    json j{{"state", phase}};
    if (t.phase != InputSelfTest::Phase::Done) return j;
    j["verdict_state"]     = healthStateName(t.verdictState);
    j["verdict"]           = t.verdict;
    j["probes"]            = t.probes;
    j["ok"]                = t.ok;
    j["reply_ms_avg"]      = t.replyMsAvg;
    j["reply_ms_max"]      = t.replyMsMax;
    j["target_enumerated"] = optionalJson(t.targetEnumerated);
    j["firmware"]          = firmwareJson(t.firmwareVersion);
    j["chip_baud"]         = optionalJson(t.chipBaud);
    j["work_mode"]         = optionalJson(t.workMode);
    j["serial_mode"]       = optionalJson(t.serialMode);
    j["finished_at"]       = t.finishedAtUnix;
    return j;
}

json baudChangeToJson(const InputBaudChange& c, bool supported) {
    if (!supported) return nullptr;
    const char* phase = c.phase == InputBaudChange::Phase::Running ? "running"
                      : c.phase == InputBaudChange::Phase::Done    ? "done" : "idle";
    json j{{"state", phase}};
    if (c.phase == InputBaudChange::Phase::Idle) return j;
    j["from_baud"] = c.fromBaud;
    j["to_baud"]   = c.toBaud;
    if (c.phase != InputBaudChange::Phase::Done) return j;
    j["ok"]          = c.ok;
    j["message"]     = c.message;
    j["finished_at"] = c.finishedAtUnix;
    return j;
}

// What the chip's datasheet lists; 115200 is its fastest.
bool isChipBaud(int baud) {
    for (int b : {1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200})
        if (b == baud) return true;
    return false;
}

} // namespace

json inputHealthToJson(const InputHealth& h) {
    return json{
        {"kind", h.kind},
        {"state", healthStateName(h.state)},
        {"summary", h.summary},
        {"device_open", optionalJson(h.deviceOpen)},
        {"target_enumerated", optionalJson(h.targetEnumerated)},
        {"firmware", firmwareJson(h.firmwareVersion)},
        {"configured_baud", optionalJson(h.configuredBaud)},
        {"chip_baud", optionalJson(h.chipBaud)},
        {"answers_at_baud", optionalJson(h.answersAtBaud)},
        {"last_hour", {
            {"checks_ok", h.checksOk},
            {"checks_failed", h.checksFailed},
            {"reply_ms_avg", h.replyMsAvg},
            {"reply_ms_max", h.replyMsMax},
            {"reconnects", h.reconnects},
            {"dropped_commands", h.droppedCommands},
        }},
    };
}

json videoHealthToJson(const VideoHealth& h) {
    json j{
        {"state", videoStateName(h.state)},
        {"last_error", h.lastError.empty() ? json(nullptr) : json(h.lastError)},
        {"reconnects", h.reconnects},
        {"retry_in_ms", nullptr},
    };
    if (h.state == VideoState::Reconnecting || h.state == VideoState::Absent) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            h.retryAt - std::chrono::steady_clock::now()).count();
        j["retry_in_ms"] = std::max<int64_t>(0, left);
    }
    return j;
}

// Reports an Owner starting an input test or speed change on a target.
static void emitHealthAction(EventBus& events, Database& db, std::string_view type,
                             const Actor& actor, int64_t targetId, json detail) {
    Event e{type, event::kOk, actor};
    e.targetId = targetId;
    if (auto row = db.getTarget(targetId)) e.targetName = row->name;
    e.detail = std::move(detail);
    events.emit(e);
}

template <bool SSL>
void registerHealthRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                          EventBus& events) {
    app.get("/api/targets/:id/health", [&auth, &db, &targets](auto* res, auto* req) {
        if (!requireRole(auth, db, res, req, Role::Viewer)) return;
        auto* target = resolveRunningTarget(targets, db, res, req, true);
        if (!target) return;
        json body = target->streamManager.withInput([](InputBackend* b) {
            if (!b) {
                // Built on the target's worker thread shortly after start.
                InputHealth none;
                none.summary = "Input isn't set up yet: the target is starting.";
                json j = inputHealthToJson(none);
                j["test"] = nullptr;
                j["baud_change"] = nullptr;
                return j;
            }
            json j = inputHealthToJson(b->health());
            j["test"] = selfTestToJson(b->selfTest(), b->hasSelfTest());
            j["baud_change"] = baudChangeToJson(b->baudChange(), b->canChangeBaud());
            return j;
        });
        body["video"] = videoHealthToJson(target->streamManager.videoHealth());
        AudioHealth audio = target->streamManager.audioHealth();
        body["audio"] = json{
            {"state", audioStateName(audio.state)},
            {"last_error", audio.lastError.empty() ? json(nullptr) : json(audio.lastError)},
        };
        res->writeHeader("Content-Type", "application/json")->end(body.dump());
    });

    app.post("/api/targets/:id/health/test", [&auth, &db, &targets, &events](auto* res, auto* req) {
        auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
        if (!authOpt) return;
        auto* target = resolveRunningTarget(targets, db, res, req, true);
        if (!target) return;
        if (!target->driverToken.empty()) {
            res->writeStatus("409 Conflict")->end("Someone is driving this target");
            return;
        }
        enum class Outcome { Started, Running, Unsupported };
        auto outcome = target->streamManager.withInput([](InputBackend* b) {
            if (!b || !b->hasSelfTest()) return Outcome::Unsupported;
            return b->startSelfTest() ? Outcome::Started : Outcome::Running;
        });
        if (outcome == Outcome::Unsupported) {
            res->writeStatus("400 Bad Request")->end("This input type has no self-test");
        } else if (outcome == Outcome::Running) {
            res->writeStatus("409 Conflict")->end("An input test is already running");
        } else {
            emitHealthAction(events, db, event::kInputTest,
                             actorOf(*authOpt, clientAddress(auth.policy, res, req)), target->id,
                             json::object());
            res->writeStatus("202 Accepted")
               ->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
        }
    });

    // Rewrites the adapter's stored line speed, then saves it on the target
    // once the adapter answers reliably at it; progress shows up in GET
    // .../health as "baud_change". The target's pipeline isn't rebuilt: the
    // running backend has already switched.
    app.post("/api/targets/:id/health/baud", [&auth, &db, &targets, &events](auto* res, auto* req) {
        auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
        if (!authOpt) return;
        auto* target = resolveRunningTarget(targets, db, res, req, true);
        if (!target) return;
        const int64_t id = target->id;
        Actor actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));

        readBody(res, 1024, [res, id, &db, &targets, &events, actor](std::string& body) {
            auto j = parseJsonOr400(res, body);
            if (!j) return;
            auto baud = jsonValueOr(*j, "baud", 0);
            if (!baud || !isChipBaud(*baud)) {
                res->writeStatus("400 Bad Request")
                   ->end("baud must be one of 1200, 2400, 4800, 9600, 14400, 19200, "
                         "38400, 57600 or 115200");
                return;
            }
            auto* t = targets.find(id);
            if (!t) { res->writeStatus("503 Service Unavailable")->end("Target is not running"); return; }
            if (!t->driverToken.empty()) {
                res->writeStatus("409 Conflict")->end("Someone is driving this target");
                return;
            }

            auto* loop = uWS::Loop::get();
            auto save = [loop, id, &db, &targets](int newBaud) {
                loop->defer([id, newBaud, &db, &targets] {
                    auto row = db.getTarget(id);
                    if (!row) return;
                    if (row->settings.serialBaud != newBaud) {
                        row->settings.serialBaud = newBaud;
                        db.updateTarget(*row);
                    }
                    // Already live, so a later apply() mustn't rebuild for it.
                    if (auto* rt = targets.find(id)) rt->appliedSettings.serialBaud = newBaud;
                });
            };
            enum class Outcome { Started, Running, Unsupported };
            auto outcome = t->streamManager.withInput([&](InputBackend* b) {
                if (!b || !b->canChangeBaud()) return Outcome::Unsupported;
                return b->startBaudChange(*baud, save) ? Outcome::Started : Outcome::Running;
            });
            if (outcome == Outcome::Unsupported) {
                res->writeStatus("400 Bad Request")->end("This input type has no adjustable speed");
            } else if (outcome == Outcome::Running) {
                res->writeStatus("409 Conflict")->end("A speed change is already running");
            } else {
                emitHealthAction(events, db, event::kBaudChange, actor, id, {{"baud", *baud}});
                res->writeStatus("202 Accepted")
                   ->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
            }
        });
    });
}

// Plain HTTP and HTTPS run the same routes.
template void registerHealthRoutes<false>(uWS::App&, Auth&, Database&, TargetManager&, EventBus&);
template void registerHealthRoutes<true>(uWS::SSLApp&, Auth&, Database&, TargetManager&, EventBus&);

} // namespace houston_kvm
