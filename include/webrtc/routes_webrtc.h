#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "core/events.h"
#include "core/target_manager.h"
#include "webrtc/selective_forwarding_unit.h"

#include <App.h>

#include <string>

namespace houston_kvm {

// Registers /api/targets/:id/webrtc/subscribe and /api/targets/:id/webrtc/answer,
// plus the original unscoped /api/webrtc/subscribe and /api/webrtc/answer,
// which address the default target; and /api/admin/ice-servers (Owner: the
// STUN/TURN servers, read-only when iceServersPinned, i.e. set by
// --ice-server).
template <bool SSL>
void registerWebrtcRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db,
                           SelectiveForwardingUnit& sfu, TargetManager& targets,
                           EventBus& events, bool iceServersPinned);

} // namespace houston_kvm
