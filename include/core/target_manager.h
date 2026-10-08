#pragma once

#include "core/database.h"
#include "core/events.h"
#include "hid/input_queue.h"
#include "video/broadcaster.h"
#include "video/stream_manager.h"
#include "webrtc/selective_forwarding_unit.h"

#include <App.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace houston_kvm {

// Every target's publisher is the sole peer in its own SFU room.
inline constexpr const char* kTargetPeerId = "target";

struct TargetRuntime;

// Follows control of targets for the audit log: each stretch of driving
// (started returns an id for it, 0 to not follow it), how many keys were
// pressed and how much mouse input was sent meanwhile, and why it ended.
// Never which keys: the hooks aren't given them, so nothing downstream can
// keep what was typed. Set once by main before any target starts; any of
// them may be left empty.
struct ControlHooks {
    std::function<int64_t(const TargetRuntime&, const Actor& driver)>                    started;
    std::function<void(int64_t session)>                                                keyPressed;
    std::function<void(int64_t session)>                                                mouse;
    std::function<void(const TargetRuntime&, int64_t session, std::string_view reason)> ended;
};

// Everything that is live for one enabled target: its capture -> encode ->
// WebRTC pipeline and input backend (StreamManager), the MJPEG fan-out, the
// input worker, and who currently holds the driver lock. Owned exclusively
// by TargetManager.
//
// Members are declared in dependency order on purpose: destruction runs in
// reverse, so the input worker (which calls into streamManager) stops first,
// then the capture/publisher threads (which call into broadcaster).
struct TargetRuntime {
    TargetRuntime(int64_t id, std::string roomId, SelectiveForwardingUnit& sfu,
                  uWS::Loop* loop, const Database::TargetSettings& settings,
                  const ControlHooks& hooks);

    const int64_t     id;
    const std::string roomId;
    std::shared_ptr<Broadcaster> broadcaster;
    StreamManager     streamManager;
    InputQueue        inputQueue;

    // Session/API token that currently holds exclusive input control of this
    // target. Touched only from the uWS event-loop thread, which is what makes
    // it safe without a lock (uWS is single-threaded per App instance).
    // Change it only through takeControl()/releaseControl(): both pair the
    // lock with the matching release of whatever keys the driver had down.
    std::string driverToken;
    // The driver's username, captured once at takeControl() time so viewers
    // can be told who's driving without a token->user DB lookup on every
    // status poll. Empty exactly when driverToken is.
    std::string driverName;
    // Who the driver is, for the audit log, and the id of their current
    // control session there (0 when nobody drives or it isn't recorded).
    Actor       driver;
    int64_t     controlSession = 0;

    // Starts from a clean keyboard/mouse state, then records `token` and
    // `who` as the driver. The caller has already checked nobody else holds
    // the lock. Re-acquiring with the token that already drives is a no-op.
    void takeControl(const std::string& token, const Actor& who);
    // Drops the lock and releases every key/button the driver may still have
    // down on the target. No-op if nobody holds it. `reason` is what the
    // audit log says ended the session: "released", "owner_override",
    // "signed_out", "credentials_revoked", "target_stopped".
    void releaseControl(std::string_view reason);

    // Counts the driver's input for the audit log: key presses, and mouse
    // moves, clicks and scrolls. Only counts; see ControlHooks.
    void recordKeyPress();
    void recordMouse();

    // What streamManager was last told to build, so a metadata-only edit
    // (rename, retag) doesn't tear down and rebuild capture + ICE.
    Database::TargetSettings appliedSettings;

private:
    const ControlHooks& hooks_;
};

// Registry of running targets. All methods run on the uWS event-loop thread
// (the same one that constructs it) — routes are its only callers — so the
// map needs no lock. TargetRuntime pointers returned by find() are valid only
// for the synchronous span of the calling handler: never capture one in an
// onData/onAborted callback or a WebSocket's per-socket data; capture the
// target id and call find() again.
//
// Stopping a target (disable, delete) can block for ~10s — StreamManager's
// destructor joins a worker that may be mid ICE-gathering — so it is handed
// to a background thread instead of stalling the event loop. A target
// started while any stop is still in flight waits for those to finish
// (startAfterRetiring), otherwise it would race the old runtime for the
// capture/serial device, hit EBUSY, and come up silently without video.
class TargetManager {
public:
    TargetManager(SelectiveForwardingUnit& sfu, Database& db, uWS::Loop* loop);
    ~TargetManager();

    TargetManager(const TargetManager&) = delete;
    TargetManager& operator=(const TargetManager&) = delete;

    // Startup: apply every target in the database.
    void loadAll();

    // Reconciles a target's runtime with its database row, after any create
    // or update: starts it if enabled and not running, rebuilds the pipeline
    // only if its hardware settings changed, stops it if disabled.
    void apply(const Database::Target& target);

    // After a target row is deleted.
    void remove(int64_t id);

    // What the unscoped /api/* routes address; 0 = none configured.
    int64_t defaultId() const { return defaultId_; }

    // nullptr if the target isn't running (unknown, disabled, or starting).
    TargetRuntime* find(int64_t id);
    bool isStarting(int64_t id) const { return startPending_.count(id) != 0; }

    // Releases the driver lock (and held keys) on every target held by this
    // token (logout: reason "signed_out").
    void releaseControlFor(const std::string& token, std::string_view reason);

    // Call before loadAll(). See ControlHooks.
    void setControlHooks(ControlHooks hooks) { hooks_ = std::move(hooks); }

    // Force-closes every /api/stream client on every target (shutdown).
    void closeAllStreams();

    // Every `intervalMs`, asks `stillValid` about each token currently
    // holding a driver lock and releases the lock of any that no longer
    // qualifies. The input hot path deliberately trusts driverToken without a
    // database lookup, so nothing else notices a driver whose session
    // expired, whose API token was revoked, whose account was deleted, or who
    // was demoted — they'd keep driving until they happened to release.
    // Call once, from the event-loop thread, after construction.
    void startDriverRevalidation(std::function<bool(const std::string&)> stillValid, int intervalMs);
    // Runs that check once, right now (after something has just revoked
    // credentials, such as a password change signing out other sessions).
    void revalidateDriversNow() { if (stillValid_) revalidateDrivers(); }
    // Stops the timer above. Call from the event-loop thread at shutdown.
    void stopBackground();

private:
    static void onRevalidateTimer(struct us_timer_t* timer);
    void revalidateDrivers();
    void retire(std::unique_ptr<TargetRuntime> runtime);
    void startAfterRetiring(int64_t id);
    void spawn(std::function<void()> job);

    SelectiveForwardingUnit& sfu_;
    Database&                db_;
    uWS::Loop*               loop_;
    ControlHooks             hooks_;

    std::unordered_map<int64_t, std::unique_ptr<TargetRuntime>> runtimes_;
    std::unordered_set<int64_t> startPending_;
    int64_t  defaultId_ = 0;
    uint64_t nextGeneration_ = 1;

    struct us_timer_t*                       revalidateTimer_ = nullptr;
    std::function<bool(const std::string&)>  stillValid_;

    // Background jobs (retiring runtimes, deferred starts). Detached, but
    // counted so the destructor can wait for all of them.
    std::mutex              jobsMutex_;
    std::condition_variable jobsCv_;
    int                     jobs_ = 0;
    int                     retiring_ = 0;
};

} // namespace houston_kvm
