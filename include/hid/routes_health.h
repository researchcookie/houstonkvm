#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "core/target_manager.h"
#include "hid/input_health.h"
#include "video/video_health.h"

#include <App.h>
#include <nlohmann/json.hpp>

namespace houston_kvm {

// Registers a target's input-link health:
//   GET  /api/targets/:id/health       Viewer+ (API tokens too): the live
//                                      state, a sentence naming which cable
//                                      to look at, and the last hour's
//                                      numbers; plus the latest self-test,
//                                      and the video's state under "video".
//   POST /api/targets/:id/health/test  Owner (browser session only): starts
//                                      an input self-test. 202 when started;
//                                      409 while someone is driving the
//                                      target or a test is already running.
//                                      Poll GET for the result. Taking
//                                      control is refused until it ends.
//
// Health carries no device paths: it's for everyone who can see the target.
template <bool SSL>
void registerHealthRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                          EventBus& events);

nlohmann::json inputHealthToJson(const InputHealth& h);
nlohmann::json videoHealthToJson(const VideoHealth& h);

} // namespace houston_kvm
