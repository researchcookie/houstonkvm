#include "video/routes_stream.h"

#include "core/http_common.h"
#include "core/target_http.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace houston_kvm {

template <bool SSL>
void registerStreamRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets) {

auto streamHandler = [&auth, &db, &targets](bool scoped) {
    return [&auth, &db, &targets, scoped](auto* res, auto* req) {
        if (!requireAuth(auth, db, res, req)) return;
        auto* target = resolveRunningTarget(targets, db, res, req, scoped);
        if (!target) return;

        res->writeStatus("200 OK")
           ->writeHeader("Content-Type",
                         "multipart/x-mixed-replace;boundary=frame")
           ->writeHeader("Cache-Control", "no-cache")
           ->writeHeader("Connection", "keep-alive");

        target->broadcaster->add(res);
        if (!target->broadcaster->latest.empty())
            Broadcaster::writeFrame(res, target->broadcaster->latest);

        // Capture the id, not the runtime: the target may be stopped or
        // deleted while this connection is open (see TargetManager).
        int64_t id = target->id;
        res->onAborted([res, &targets, id]() {
            if (auto* t = targets.find(id)) t->broadcaster->remove(res);
        });
    };
};

auto snapshotHandler = [&auth, &db, &targets](bool scoped) {
    return [&auth, &db, &targets, scoped](auto* res, auto* req) {
        if (!requireAuth(auth, db, res, req)) return;
        auto* target = resolveRunningTarget(targets, db, res, req, scoped);
        if (!target) return;

        auto frame = target->streamManager.withCapture(
            [](CaptureSource* capture) -> std::optional<std::vector<uint8_t>> {
                if (!capture) return std::nullopt;
                return capture->snapshot();
            });
        if (!frame) {
            res->writeStatus("503 Service Unavailable")
               ->end("No capture device");
            return;
        }
        if (frame->empty()) {
            res->writeStatus("503 Service Unavailable")
               ->end("No frame available yet");
            return;
        }
        res->writeHeader("Content-Type", "image/jpeg")
           ->writeHeader("Cache-Control", "no-store")
           ->writeHeader("Content-Length", std::to_string(frame->size()))
           ->end(std::string_view(
               reinterpret_cast<const char*>(frame->data()), frame->size()));
    };
};

app.get("/api/stream", streamHandler(false))
   .get("/api/targets/:id/stream", streamHandler(true))
   .get("/api/snapshot", snapshotHandler(false))
   .get("/api/targets/:id/snapshot", snapshotHandler(true));

}

// Plain HTTP and HTTPS run the same routes.
template void registerStreamRoutes<false>(uWS::App&, Auth&, Database&, TargetManager&);
template void registerStreamRoutes<true>(uWS::SSLApp&, Auth&, Database&, TargetManager&);

} // namespace houston_kvm
