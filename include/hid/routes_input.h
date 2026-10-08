#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "core/target_manager.h"
#include "hid/input_queue.h"
#include "video/stream_manager.h"

#include <App.h>
#include <nlohmann/json.hpp>

#include <string>

namespace houston_kvm {

// Registers control and input for each target:
//   POST /api/targets/:id/control/acquire | /control/release,
//   GET  /api/targets/:id/control/status,
//   POST /api/targets/:id/input, and WS /api/targets/:id/input/ws,
// plus unscoped /api/control/*, /api/input and /api/input/ws for the
// default target. Driving needs exactly the Operator role: an Owner isn't a
// driver, so administering the server and piloting a target stay separate.
// An Owner can only force-release a stuck driver lock.
//
// Control is per target: a session may drive several at once (each is its
// own lock), and one driver never blocks another target. The lock lives on
// the target (TargetRuntime::driverToken). Every way it can end — release,
// takeover, logout, the target stopping, the driver's credentials lapsing —
// also releases whatever keys and buttons that driver had down, and so does
// the driver's input socket closing (which keeps the lock but clears the
// keys): the HID chip holds its last report, so otherwise a key down at that
// moment would stay down on the target.
//
// The unscoped routes resolve the default afresh on every request, so if an
// admin designates a different default, a driver of the old one simply stops
// being a driver there rather than silently steering the new machine.
template <bool SSL>
void registerInputRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                         EventBus& events);

// Shared by both the HTTP and WebSocket input handlers so the
// mousemove/mousebutton/mousescroll/key/text dispatch logic (and the
// isReady() gate) lives in exactly one place. Does not itself check
// authorization/driver status — callers must do that first, since the two
// transports authenticate differently (HTTP re-derives the token per
// request, the WS connection carries it in PerSocketData).
enum class DispatchStatus { Ok, BadRequest, UnknownType, Unavailable, Overloaded };

struct DispatchResult {
    DispatchStatus status;
    std::string    message; // human-readable reason, empty on Ok
};

// The largest input message either transport accepts. Mouse and key events
// are well under 100 bytes; this is sized for a `text` message holding the
// most the queue will take (InputQueue::kMaxPendingText), with room for
// every character to arrive JSON-escaped ("\n", "\"", "\\").
constexpr unsigned kMaxInputMessageBytes = 2 * InputQueue::kMaxPendingText + 1024;

// Also counts accepted input for the target's audit hooks: key presses and
// mouse events, never which key.
DispatchResult dispatchInputMessage(const nlohmann::json& j, TargetRuntime& target);

} // namespace houston_kvm
