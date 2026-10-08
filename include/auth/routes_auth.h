#pragma once

#include "auth/auth.h"
#include "auth/password_hasher.h"
#include "core/target_manager.h"
#include "core/database.h"
#include "core/events.h"
#include "auth/rate_limiter.h"

#include <App.h>

namespace houston_kvm {

// Registers /api/status, /api/setup, /api/login, /api/logout, /api/me,
// /api/tokens (POST/GET/DELETE), /api/users (POST/GET/PUT role/DELETE),
// and /api/account/password.
//
// Each sign-in, account and token change is reported on `events` (see
// core/events.h), which is how it reaches the audit log.
//
// Password hashing and verification run on `hasher`'s threads, never on the
// event loop (see PasswordHasher).
//
// secureCookies controls whether plain-HTTP Set-Cookie responses carry the
// `Secure` attribute — set it when a reverse proxy terminates TLS in front
// of the server. Over the server's own HTTPS the cookie is always Secure
// (see kSessionCookie).
template <bool SSL>
void registerAuthRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                         LoginRateLimiter& loginLimiter, PasswordHasher& hasher, EventBus& events,
                         bool secureCookies);

} // namespace houston_kvm
