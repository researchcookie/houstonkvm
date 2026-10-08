#include "webrtc/routes_webrtc.h"

#include "core/http_common.h"
#include "core/target_http.h"

#include <sodium.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace houston_kvm {

using json = nlohmann::json;

template <bool SSL>
void registerWebrtcRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db,
                           SelectiveForwardingUnit& sfu, TargetManager& targets,
                           EventBus& events, bool iceServersPinned) {

// Registration runs on the event-loop thread, so this is that loop.
uWS::Loop* loop = uWS::Loop::get();

auto subscribeHandler = [&auth, &db, &sfu, &targets, loop](bool scoped) {
    return [&auth, &db, &sfu, &targets, loop, scoped](auto* res, auto* req) {
        auto uidOpt = requireAuth(auth, db, res, req);
        if (!uidOpt) return;
        auto* target = resolveRunningTarget(targets, db, res, req, scoped);
        if (!target) return;
        // ?candidates=on-answer: the server's ICE candidates come back from
        // /webrtc/answer instead of in the offer (see createSubscription()).
        // Without it, the original flow, for clients written against it.
        const bool deferCandidates = req->getQuery("candidates") == "on-answer";

        uint8_t raw[16];
        randombytes_buf(raw, sizeof(raw));
        static constexpr char HEX[] = "0123456789abcdef";
        std::string subscriberId(sizeof(raw) * 2, '\0');
        for (size_t i = 0; i < sizeof(raw); ++i) {
            subscriberId[2 * i]     = HEX[raw[i] >> 4];
            subscriberId[2 * i + 1] = HEX[raw[i] & 0x0F];
        }

        // The offer arrives once ICE gathering finishes, on a libdatachannel
        // thread; everything else on this loop (every target's video, input
        // and API) carries on meanwhile. `gone` is only touched on the loop.
        auto gone = std::make_shared<bool>(false);
        res->onAborted([gone]() { *gone = true; });
        // The browser uses the same STUN/TURN servers as the server's end.
        auto iceServers = iceServersForBrowser(sfu.iceServers());
        sfu.createSubscription(target->roomId, subscriberId, *uidOpt, kTargetPeerId, deferCandidates,
            [loop, res, gone, subscriberId, iceServers](std::string offer) {
                loop->defer([res, gone, subscriberId, iceServers, offer = std::move(offer)]() {
                    // A viewer who left leaves a subscription nobody will
                    // answer; the SFU closes it after kSubscriptionConnectTimeout.
                    if (*gone) return;
                    res->cork([res, &subscriberId, &offer, &iceServers]() {
                        if (offer.empty()) {
                            res->writeStatus("503 Service Unavailable")
                               ->end("WebRTC publisher not available");
                            return;
                        }
                        res->writeHeader("Content-Type", "application/json")
                           ->end(json{{"subscriberId", subscriberId}, {"sdp", offer},
                                      {"iceServers", iceServers}}.dump());
                    });
                });
            });
    };
};

auto answerHandler = [&auth, &db, &sfu, &targets, loop](bool scoped) {
    return [&auth, &db, &sfu, &targets, loop, scoped](auto* res, auto* req) {
        auto uidOpt = requireAuth(auth, db, res, req);
        if (!uidOpt) return;
        int64_t uid = *uidOpt;
        // Resolved now, while req is still valid; the room name is copied
        // out because the runtime itself must not be held across onData.
        auto* target = resolveRunningTarget(targets, db, res, req, scoped);
        if (!target) return;
        std::string roomId = target->roomId;

        readBody(res, 16384, [res, &sfu, loop, roomId, uid](std::string& body) {
            auto j = json::parse(body, nullptr, false);
            if (j.is_discarded()) {
                res->writeStatus("400 Bad Request")->end("Invalid JSON");
                return;
            }
            auto subscriberIdOpt = jsonValueOr(j, "subscriberId", std::string{});
            auto sdpOpt          = jsonValueOr(j, "sdp", std::string{});
            if (!subscriberIdOpt || !sdpOpt) {
                res->writeStatus("400 Bad Request")->end("Invalid field types");
                return;
            }
            const std::string& subscriberId = *subscriberIdOpt;
            const std::string& sdp          = *sdpOpt;
            if (subscriberId.empty() || sdp.empty()) {
                res->writeStatus("400 Bad Request")
                   ->end("Missing subscriberId or sdp");
                return;
            }
            // The response waits for the server's candidates, which may still
            // be gathering; as with subscribe, nothing else waits with it.
            auto gone = std::make_shared<bool>(false);
            res->onAborted([gone]() { *gone = true; });
            auto respond = [loop, res, gone](std::optional<std::vector<rtc::Candidate>> candidates) {
                loop->defer([res, gone, candidates = std::move(candidates)]() {
                    if (*gone) return;
                    res->cork([res, &candidates]() {
                        if (!candidates) {
                            res->writeStatus("503 Service Unavailable")
                               ->end("The server's ICE candidates aren't ready (subscribe again)");
                            return;
                        }
                        json list = json::array();
                        for (const auto& c : *candidates)
                            list.push_back({{"candidate", c.candidate()}, {"mid", c.mid()}});
                        res->writeHeader("Content-Type", "application/json")
                           ->end(json{{"ok", true}, {"candidates", list}}.dump());
                    });
                });
            };
            try {
                if (!sfu.setSubscribeAnswer(roomId, subscriberId, uid, kTargetPeerId, sdp, respond)) {
                    res->writeStatus("404 Not Found")
                       ->end("No such subscription (it may have expired; subscribe again)");
                    return;
                }
            } catch (const std::exception&) {
                res->writeStatus("400 Bad Request")->end("Invalid SDP");
                return;
            }
        });
    };
};

auto iceServersState = [&sfu, iceServersPinned]() {
    return json{{"ice_servers", iceServersToJson(sfu.iceServers())},
                {"managed_by_config", iceServersPinned}};
};

app.get("/api/admin/ice-servers", [&auth, &db, iceServersState](auto* res, auto* req) {
    if (!requireRole(auth, db, res, req, Role::Owner)) return;
    res->writeHeader("Content-Type", "application/json")->end(iceServersState().dump());
});

// Replaces the whole list. Viewers connecting from then on use it; viewers
// already connected keep the servers they connected with.
app.put("/api/admin/ice-servers", [&auth, &db, &sfu, &events, iceServersPinned,
                                   iceServersState](auto* res, auto* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;
    if (iceServersPinned) {
        res->writeStatus("409 Conflict")
           ->end("The STUN/TURN servers are set in the server's configuration (--ice-server); "
                 "change them there");
        return;
    }
    Actor actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));

    readBody(res, 16384, [res, &db, &sfu, &events, actor, iceServersState](std::string& body) {
        auto j = parseJsonOr400(res, body);
        if (!j) return;
        auto it = j->find("ice_servers");
        if (!j->is_object() || it == j->end()) {
            res->writeStatus("400 Bad Request")->end("Expected {\"ice_servers\": [...]}");
            return;
        }
        std::string error;
        auto servers = iceServersFromJson(*it, error);
        if (!servers) {
            res->writeStatus("400 Bad Request")->end(error);
            return;
        }

        auto before = sfu.iceServers();
        DbOutcome outcome;
        if (!db.setIceServersJson(iceServersToJson(*servers).dump(), &outcome)) {
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        sfu.setIceServers(*servers);

        // Addresses only: TURN passwords stay out of the audit log.
        auto urls = [](const std::vector<IceServerConfig>& list) {
            json out = json::array();
            for (const auto& s : list) out.push_back(s.url);
            return out;
        };
        if (before != *servers) {
            Event e{event::kIceServers, event::kOk, actor};
            e.detail = {{"changes", {{"ice_servers", {{"from", urls(before)}, {"to", urls(*servers)}}}}}};
            events.emit(e);
        }
        res->writeHeader("Content-Type", "application/json")->end(iceServersState().dump());
    });
});

app.post("/api/webrtc/subscribe", subscribeHandler(false))
   .post("/api/targets/:id/webrtc/subscribe", subscribeHandler(true))
   .post("/api/webrtc/answer", answerHandler(false))
   .post("/api/targets/:id/webrtc/answer", answerHandler(true));

}

// Plain HTTP and HTTPS run the same routes.
template void registerWebrtcRoutes<false>(uWS::App&, Auth&, Database&, SelectiveForwardingUnit&,
                                          TargetManager&, EventBus&, bool);
template void registerWebrtcRoutes<true>(uWS::SSLApp&, Auth&, Database&, SelectiveForwardingUnit&,
                                         TargetManager&, EventBus&, bool);

} // namespace houston_kvm
