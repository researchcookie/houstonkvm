#include "hid/routes_input.h"

#include "core/http_common.h"
#include "core/target_http.h"
#include "hid/text_typing.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace houston_kvm {

using json = nlohmann::json;

namespace {
// Per connection on /api/input/ws and /api/targets/:id/input/ws: the token
// from the upgrade, compared with the target's driverToken on every message,
// since driving can change while the socket stays open. targetId 0 is the
// unscoped route, which follows whichever target is the default at each
// message. An id, not a pointer: the target can stop while the socket lives.
struct InputSocketData {
    std::string token;
    int64_t     targetId = 0;
};
} // namespace

template <bool SSL>
void registerInputRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                         EventBus& events) {

// Each route exists twice: /api/targets/:id/... (scoped — :id is parameter 0)
// and the original unscoped path, which addresses the default target. The
// lambdas below take `scoped` and are instantiated for both.

// Running target a socket's id/default points at, or nullptr. Valid only for
// the synchronous span of the calling handler — see TargetManager::find.
auto runningTarget = [&targets](int64_t boundId) {
    return targets.find(boundId != 0 ? boundId : targets.defaultId());
};

auto acquire = [&auth, &db, &targets](bool scoped) {
    return [&auth, &db, &targets, scoped](auto* res, auto* req) {
        auto authOpt = requireExactRole(auth, db, res, req, Role::Operator);
        if (!authOpt) return;
        const auto& token = authOpt->token;

        auto* target = resolveRunningTarget(targets, db, res, req, scoped);
        if (!target) return;
        bool ready = target->streamManager.withInput([](InputBackend* b) { return b && b->isReady(); });
        if (!ready) {
            res->writeStatus("503 Service Unavailable")->end("Input injection unavailable");
            return;
        }
        if (!target->driverToken.empty() && target->driverToken != token) {
            res->writeStatus("409 Conflict")->end("Another session is driving");
            return;
        }
        // Keeps a test's measurements clean, and real input out of it and
        // out of a speed change, which resets the adapter.
        const char* busy = target->streamManager.withInput([](InputBackend* b) -> const char* {
            if (!b) return nullptr;
            if (b->selfTest().phase == InputSelfTest::Phase::Running)
                return "An input test is running";
            if (b->baudChange().phase == InputBaudChange::Phase::Running)
                return "The input adapter's speed is being changed";
            return nullptr;
        });
        if (busy) {
            res->writeStatus("409 Conflict")->end(busy);
            return;
        }
        target->takeControl(token, actorOf(*authOpt, clientAddress(auth.policy, res, req)));
        res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
    };
};

auto release = [&auth, &db, &targets, &events, runningTarget](bool scoped) {
    return [&auth, &db, &targets, &events, runningTarget, scoped](auto* res, auto* req) {
        auto authOpt = requireRole(auth, db, res, req, Role::Viewer);
        if (!authOpt) return;
        // An Owner clearing someone else's lock is recorded as the Owner's
        // own action, besides the driver's session ending.
        auto forceRelease = [&](TargetRuntime& target) {
            Event e{event::kControlForced};
            e.actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));
            e.targetId = target.id;
            if (auto row = db.getTarget(target.id)) e.targetName = row->name;
            e.detail = {{"driver", target.driverName}, {"control_session", target.controlSession}};
            events.emit(e);
            target.releaseControl("owner_override");
        };

        if (scoped) {
            auto id = resolveTargetId(targets, res, req, true);
            if (!id) return;
            if (auto* target = targets.find(*id)) {
                // The driver, or an Owner overriding a stuck lock.
                if (target->driverToken == authOpt->token)
                    target->releaseControl("released");
                else if (authOpt->role >= Role::Owner && !target->driverToken.empty())
                    forceRelease(*target);
            } else if (!db.getTarget(*id)) {
                res->writeStatus("404 Not Found")->end("Unknown target");
                return;
            } // else: known but not running — nothing is held, so nothing to release
        } else {
            // Drop this caller's lock wherever it is (covers a default that
            // was re-designated while they were driving), then let an Owner
            // also force-clear whoever holds the current default.
            targets.releaseControlFor(authOpt->token, "released");
            if (authOpt->role >= Role::Owner)
                if (auto* target = runningTarget(0); target && !target->driverToken.empty())
                    forceRelease(*target);
        }
        res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
    };
};

auto status = [&auth, &db, &targets, runningTarget](bool scoped) {
    return [&auth, &db, &targets, runningTarget, scoped](auto* res, auto* req) {
        auto authOpt = requireRole(auth, db, res, req, Role::Viewer);
        if (!authOpt) return;

        TargetRuntime* target = nullptr;
        if (scoped) {
            auto id = resolveTargetId(targets, res, req, true);
            if (!id) return;
            target = targets.find(*id);
            if (!target && !db.getTarget(*id)) {
                res->writeStatus("404 Not Found")->end("Unknown target");
                return;
            }
        } else {
            // Polled by the UI every few seconds, so "no default target" is
            // an ordinary answer (nobody's driving), not an error.
            target = runningTarget(0);
        }
        bool controlled = target && !target->driverToken.empty();
        bool isDriver   = target && target->driverToken == authOpt->token;
        // Driver's name goes to any Viewer+ caller (same visibility as
        // `controlled` already had) so other viewers can be told who's
        // driving, not just that someone is.
        std::string driver = controlled ? target->driverName : "";
        size_t typing = target ? target->inputQueue.textRemaining() : 0;
        res->writeHeader("Content-Type", "application/json")
           ->end(json{{"controlled", controlled}, {"is_driver", isDriver}, {"driver", driver},
                      {"typing_remaining", typing}}.dump());
    };
};

auto input = [&auth, &db, &targets](bool scoped) {
    return [&auth, &db, &targets, scoped](auto* res, auto* req) {
        // Hot path: every input event lands here, so no database round trip.
        // driverToken is only ever set from a token checked at acquire and
        // cleared when that lapses, so a match proves an authorized driver.
        // Otherwise the full check runs, for the right 401 or 403.
        // The match below skips authenticate(), so its Origin check with it.
        if (!sameOriginOr403(auth.policy, res, req)) return;
        std::string token = extractToken<SSL>(req);
        int64_t id = scoped ? parseId(req->getParameter(0)).value_or(0) : targets.defaultId();
        auto* target = id != 0 ? targets.find(id) : nullptr;
        if (!target || target->driverToken.empty() || token != target->driverToken) {
            auto authOpt = requireExactRole(auth, db, res, req, Role::Operator);
            if (!authOpt) return;
            // Authorized, just not driving: say why (bad id, unknown,
            // disabled, starting) before falling back to "not the driver".
            if (!resolveRunningTarget(targets, db, res, req, scoped)) return;
            res->writeStatus("403 Forbidden")->end("Not the driver");
            return;
        }
        readBody(res, kMaxInputMessageBytes, [res, token = std::move(token), id, &targets](std::string& body) {
            // Re-resolved (not captured): the target can be stopped, deleted
            // or re-designated between the headers and the end of the body,
            // and control may have been released in that gap too.
            auto* target = targets.find(id);
            if (!target || target->driverToken.empty() || token != target->driverToken) {
                res->writeStatus("403 Forbidden")->end("Not the driver");
                return;
            }
            auto j = json::parse(body, nullptr, false);
            if (j.is_discarded()) {
                res->writeStatus("400 Bad Request")->end("Invalid JSON");
                return;
            }
            auto result = dispatchInputMessage(j, *target);
            switch (result.status) {
                case DispatchStatus::Unavailable:
                case DispatchStatus::Overloaded:
                    res->writeStatus("503 Service Unavailable")->end(result.message);
                    return;
                case DispatchStatus::BadRequest:
                case DispatchStatus::UnknownType:
                    res->writeStatus("400 Bad Request")->end(result.message);
                    return;
                case DispatchStatus::Ok:
                    break;
            }
            res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
        });
    };
};

// Browser-only counterpart to POST /input: one connection instead of a
// request per event. Cookie sessions only, since a browser's WebSocket
// handshake can't carry an Authorization header; API clients use the HTTP
// route. The Operator role is checked once, at upgrade; whether this
// connection is still the driver is checked on every message, as the HTTP
// route does.
auto socketBehavior = [&auth, &db, &targets, runningTarget](bool scoped) {
    return typename uWS::TemplatedApp<SSL>::template WebSocketBehavior<InputSocketData>{
        .compression = uWS::DISABLED,
        .maxPayloadLength = kMaxInputMessageBytes, // matches the HTTP route's body cap above
        .upgrade = [&auth, &db, &targets, scoped](auto* res, auto* req, auto* context) {
            auto authOpt = requireExactRole(auth, db, res, req, Role::Operator);
            if (!authOpt) return; // requireExactRole already wrote 401/403
            int64_t boundId = 0;
            if (scoped) {
                // Refuse the upgrade outright for a target that can't take
                // input, rather than opening a socket that only ever says no.
                auto* target = resolveRunningTarget(targets, db, res, req, true);
                if (!target) return;
                boundId = target->id;
            }
            res->template upgrade<InputSocketData>(
                InputSocketData{authOpt->token, boundId},
                req->getHeader("sec-websocket-key"),
                req->getHeader("sec-websocket-protocol"),
                req->getHeader("sec-websocket-extensions"),
                context);
        },
        .message = [runningTarget](auto* ws, std::string_view msg, uWS::OpCode) {
            auto* data = static_cast<InputSocketData*>(ws->getUserData());
            auto* target = runningTarget(data->targetId);
            if (!target || target->driverToken.empty() || data->token != target->driverToken) {
                ws->send(R"({"ok":false,"reason":"not_driver"})", uWS::OpCode::TEXT);
                return;
            }
            auto j = json::parse(msg, nullptr, false);
            if (j.is_discarded()) {
                ws->send(R"({"ok":false,"reason":"bad_json"})", uWS::OpCode::TEXT);
                return;
            }
            auto result = dispatchInputMessage(j, *target);
            if (result.status != DispatchStatus::Ok)
                ws->send(json{{"ok", false}, {"reason", result.message}}.dump(), uWS::OpCode::TEXT);
            // Ok: no ack sent — the client already ignores 200 responses on the
            // HTTP path today, so there's nothing gained by echoing success.
        },
        .close = [runningTarget](auto* ws, int, std::string_view) {
            // The browser tab closed or the connection dropped. If this was
            // the live driver it may well have had keys or a button down
            // (the keyup will never arrive), so clear them now — but keep the
            // lock: a network blip shouldn't cost the driver their seat, and
            // the UI reopens the socket on its own while it still holds it.
            auto* data = static_cast<InputSocketData*>(ws->getUserData());
            auto* target = runningTarget(data->targetId);
            if (target && !target->driverToken.empty() && data->token == target->driverToken)
                target->inputQueue.pushReleaseAll();
        },
    };
};

app.post("/api/control/acquire", acquire(false))
   .post("/api/targets/:id/control/acquire", acquire(true))
   .post("/api/control/release", release(false))
   .post("/api/targets/:id/control/release", release(true))
   .get("/api/control/status", status(false))
   .get("/api/targets/:id/control/status", status(true))
   .post("/api/input", input(false))
   .post("/api/targets/:id/input", input(true))
   .template ws<InputSocketData>("/api/input/ws", socketBehavior(false))
   .template ws<InputSocketData>("/api/targets/:id/input/ws", socketBehavior(true));

}

DispatchResult dispatchInputMessage(const json& j, TargetRuntime& target) {
    StreamManager& streamManager = target.streamManager;
    InputQueue&    inputQueue    = target.inputQueue;
    bool ready = streamManager.withInput([](InputBackend* b) { return b && b->isReady(); });
    if (!ready) return {DispatchStatus::Unavailable, "Input injection unavailable"};

    auto typeOpt = jsonValueOr(j, "type", std::string{});
    if (!typeOpt) return {DispatchStatus::BadRequest, "Invalid field types"};
    const std::string& type = *typeOpt;

    if (type == "mousemove") {
        auto x = jsonValueOr(j, "x", 0.0), y = jsonValueOr(j, "y", 0.0);
        if (!x || !y) return {DispatchStatus::BadRequest, "Invalid field types"};
        inputQueue.pushMouseMove(*x, *y); // coalesced, always delivered — no Overloaded case
        target.recordMouse();
        return {DispatchStatus::Ok, ""};
    }

    bool queued;
    if (type == "mousebutton") {
        auto button  = jsonValueOr(j, "button", 0);
        auto pressed = jsonValueOr(j, "pressed", false);
        if (!button || !pressed) return {DispatchStatus::BadRequest, "Invalid field types"};
        queued = inputQueue.pushMouseButton(*button, *pressed);
    } else if (type == "mousescroll") {
        auto delta = jsonValueOr(j, "delta", 0);
        if (!delta) return {DispatchStatus::BadRequest, "Invalid field types"};
        queued = inputQueue.pushMouseScroll(*delta);
    } else if (type == "key") {
        auto code    = jsonValueOr(j, "code", std::string{});
        auto pressed = jsonValueOr(j, "pressed", false);
        if (!code || !pressed) return {DispatchStatus::BadRequest, "Invalid field types"};
        queued = inputQueue.pushKey(*code, *pressed);
        if (queued && *pressed) target.recordKeyPress();   // counted, never which key
    } else if (type == "text") {
        auto text = jsonValueOr(j, "text", std::string{});
        if (!text) return {DispatchStatus::BadRequest, "Invalid field types"};
        auto prepared = prepareTextForTyping(*text);
        if (!prepared.error.empty()) return {DispatchStatus::BadRequest, prepared.error};
        if (!inputQueue.pushText(prepared.text))
            return {DispatchStatus::Overloaded, "Too much text is already waiting to be typed"};
        // One key press per character queued, like typing it by hand: the
        // audit log counts, never records what.
        for (size_t i = 0; i < prepared.text.size(); ++i) target.recordKeyPress();
        return {DispatchStatus::Ok, ""};
    } else if (type == "text_cancel") {
        inputQueue.cancelText();
        return {DispatchStatus::Ok, ""};
    } else {
        return {DispatchStatus::UnknownType, "Unknown input type"};
    }
    if (!queued) return {DispatchStatus::Overloaded, "Input queue full"};
    if (type != "key") target.recordMouse();
    return {DispatchStatus::Ok, ""};
}

// Plain HTTP and HTTPS run the same routes.
template void registerInputRoutes<false>(uWS::App&, Auth&, Database&, TargetManager&, EventBus&);
template void registerInputRoutes<true>(uWS::SSLApp&, Auth&, Database&, TargetManager&, EventBus&);

} // namespace houston_kvm
