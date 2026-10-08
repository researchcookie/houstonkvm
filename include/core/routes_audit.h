#pragma once

#include "auth/auth.h"
#include "core/audit_log.h"
#include "core/database.h"

#include <App.h>

namespace houston_kvm {

// Registers the Owner-only audit log API (see core/audit_log.h):
//   GET /api/audit/events            — what happened, newest first
//   GET /api/audit/control-sessions  — who drove which target, when, and
//                                      how many keys/mouse events (counts only)
// Both a browser session and an API token work, so a collector can read
// the log.
template <bool SSL>
void registerAuditRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, AuditLog& audit);

} // namespace houston_kvm
