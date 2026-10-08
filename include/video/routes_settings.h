#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "core/target_manager.h"

#include <App.h>

namespace houston_kvm {

// Registers /api/settings (per-user UI preferences: GET/PUT) and the
// per-target capture settings, each in an unscoped form (addresses the
// default target) and a /api/targets/:id/... form:
//   /api/video/settings (capture resolution/fps + WebRTC bitrate, Operator+:
//     GET/PUT — a narrower view of the target's config, so an Operator can
//     resize/retune the shared stream without the device-path/serial fields
//     that stay Owner-only; GET also reports the live negotiated
//     resolution/fps under "active", not just the saved request),
//   /api/video/capabilities (Operator+, GET-only: the resolution/fps modes
//     the capture hardware itself reports via VIDIOC_ENUM_FRAMESIZES/
//     FRAMEINTERVALS, so the settings UI can offer only combinations the
//     device actually supports — accepts an optional ?device= override for
//     previewing a not-yet-saved device).
// Also the legacy /api/admin/settings (Owner-only GET/PUT of the default
// target's full hardware wiring; superseded by PUT /api/targets/:id) and
// /api/admin/devices (Owner-only, GET-only: the /dev/video* capture nodes
// and USB-serial adapters actually present on the system, each with the
// target already using it, so setup doesn't require already knowing Linux
// device-node paths).
template <bool SSL>
void registerSettingsRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                            EventBus& events);

} // namespace houston_kvm
