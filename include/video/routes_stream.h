#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "core/target_manager.h"

#include <App.h>

namespace houston_kvm {

// Registers /api/targets/:id/stream (MJPEG multipart) and
// /api/targets/:id/snapshot, plus the original unscoped /api/stream and
// /api/snapshot, which address the default target.
template <bool SSL>
void registerStreamRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets);

} // namespace houston_kvm
