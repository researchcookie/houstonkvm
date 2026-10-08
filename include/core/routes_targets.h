#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "core/events.h"
#include "core/target_manager.h"

#include <App.h>
#include <nlohmann/json.hpp>

#include <string>

namespace houston_kvm {

// Registers the target directory and its admin management:
//
//   GET    /api/targets            Viewer+   list/search — ?q= (whitespace-
//                                  separated terms, all must match a name,
//                                  description, group or tag), ?tag=, ?group=,
//                                  ?enabled=1.
//   GET    /api/targets/facets     Viewer+   existing groups and tags with
//                                  counts, for pickers/autocomplete so admins
//                                  reuse a spelling instead of inventing one.
//   GET    /api/targets/:id        Viewer+
//   POST   /api/targets            Owner (browser session only)
//   PUT    /api/targets/:id        Owner (browser session only) — partial:
//                                  only the fields sent change.
//   DELETE /api/targets/:id        Owner (browser session only)
//
// Hardware wiring (device paths, serial baud) is only returned to an Owner
// browser session, never to Viewers/Operators or API tokens.
// For the audit log: {"field": {"from": old, "to": new}} for each field an
// update changed (empty if none), and reporting an event about a target.
nlohmann::json auditChanges(const Database::Target& before, const Database::Target& after);
void emitTarget(EventBus& events, std::string_view type, const Actor& actor,
                const Database::Target& t, nlohmann::json detail);

template <bool SSL>
void registerTargetRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                          EventBus& events);

// Applies whichever hardware/capture fields are present in `j` (v4l2_device,
// capture_width/height/fps, qmp_socket, serial_device, serial_baud,
// webrtc_bitrate_kbps) onto `settings`, leaving absent ones as they were.
// Shared with the legacy /api/admin/settings and /api/video/settings routes
// so every path enforces identical limits and error messages. Returns ""
// on success, otherwise a message naming the bad field.
std::string applySettingsFields(const nlohmann::json& j, Database::TargetSettings& settings);

// JSON keys for a target's hardware wiring — same names the original
// /api/admin/settings used, so existing clients keep working.
nlohmann::json targetSettingsToJson(const Database::TargetSettings& settings);

// Turns a failed insert/update's DbOutcome into a user-facing conflict
// message (which name/device is already taken), or "" if it isn't one.
std::string describeTargetConflict(const DbOutcome& outcome);

} // namespace houston_kvm
