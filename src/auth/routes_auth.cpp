#include "auth/routes_auth.h"

#include "core/http_common.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace houston_kvm {

using json = nlohmann::json;

// Returns true if targetId is currently the only owner (so demoting/deleting
// it must be blocked); std::nullopt on a DB error (caller should 500).
static std::optional<bool> isLastOwner(Database& db, int64_t targetId) {
    Stmt cnt(db.handle(), "SELECT COUNT(*) FROM users WHERE role = 'owner' AND id != ?");
    cnt.bind(1, static_cast<sqlite3_int64>(targetId));
    if (cnt.step() == Stmt::StepResult::Error) return std::nullopt;
    return cnt.column_int(0) == 0;
}

// Runs `work` (password hashing or verification) on a PasswordHasher
// thread, then `done` back on the event loop. `done` always runs, so rate
// limiting and other bookkeeping can't be dodged by hanging up mid-check;
// it is handed nullptr instead of the response once the client has gone.
// Replies 503 at once when too many checks are already queued.
template <bool SSL>
using Reply = uWS::HttpResponse<SSL>*;

static void emitAuth(EventBus& events, std::string_view type, std::string_view outcome,
                     Actor actor, nlohmann::json detail = nlohmann::json::object()) {
    Event e{type, outcome, std::move(actor)};
    e.detail = std::move(detail);
    events.emit(e);
}
template <bool SSL>
static void offLoop(PasswordHasher& hasher, Reply<SSL> res,
                    std::function<void()> work,
                    std::type_identity_t<std::function<void(Reply<SSL>)>> done,
                    std::function<void()> onBusy = {}) {
    auto aborted = std::make_shared<bool>(false);
    res->onAborted([aborted] { *aborted = true; });
    bool queued = hasher.submit(std::move(work), [res, aborted, done = std::move(done)] {
        if (*aborted) { done(nullptr); return; }
        res->cork([&] { done(res); });
    });
    if (queued) return;
    if (onBusy) onBusy();
    res->writeStatus("503 Service Unavailable")
       ->writeHeader("Retry-After", "1")
       ->end("Server busy checking passwords, try again");
}

template <bool SSL>
static void handleStatus(Auth& auth, Database& db,
                          uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    Stmt s(db.handle(), "SELECT COUNT(*) FROM users");
    auto step = s.step();
    if (step == Stmt::StepResult::Error) {
        res->writeStatus("500 Internal Server Error")->end(s.outcome().message);
        return;
    }
    bool needs_setup = (step == Stmt::StepResult::Row) && (s.column_int(0) == 0);

    bool authenticated = false;
    auto token = getCookie(req->getHeader("cookie"), kSessionCookie<SSL>);
    if (!token.empty()) {
        DbOutcome outcome;
        authenticated = auth.verifySession(db, token, outcome).has_value();
    }

    res->writeHeader("Content-Type", "application/json")
       ->end(json{{"needs_setup", needs_setup},
                  {"authenticated", authenticated}}.dump());
}

// The "no accounts yet" check is repeated after hashing: two setup requests
// racing through the (slow, off-loop) hash would otherwise both pass it and
// create two Owners.
static std::optional<bool> needsSetup(Database& db) {
    Stmt check(db.handle(), "SELECT COUNT(*) FROM users");
    auto step = check.step();
    if (step == Stmt::StepResult::Error) {
        std::cerr << "Setup check error: " << check.outcome().message << "\n";
        return std::nullopt;
    }
    return step == Stmt::StepResult::Row && check.column_int(0) == 0;
}

template <bool SSL>
static void handleSetup(Auth& auth, Database& db, LoginRateLimiter& loginLimiter,
                         PasswordHasher& hasher, EventBus& events, bool secureCookies,
                         uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    if (!sameOriginOr403(auth.policy, res, req)) return;
    auto ip = clientAddress(auth.policy, res, req);
    auto ua = std::string(req->getHeader("user-agent"));

    readBody(res, 4096, [res, &auth, &db, &loginLimiter, &hasher, &events, secureCookies,
                          ip = std::move(ip), ua = std::move(ua)](std::string& body) mutable {
        auto open = needsSetup(db);
        if (!open) { res->writeStatus("500 Internal Server Error")->end("Server error"); return; }
        if (!*open) { res->writeStatus("409 Conflict")->end("Setup already complete"); return; }

        auto jOpt = parseJsonOr400(res, body);
        if (!jOpt) return;
        auto& j = *jOpt;

        int wait = loginLimiter.checkLocked(ip);
        if (wait > 0) {
            res->writeStatus("429 Too Many Requests")
               ->writeHeader("Retry-After", std::to_string(wait))
               ->end("Too many wrong setup codes, try again later");
            return;
        }
        auto codeOpt = jsonValueOr(j, "setup_code", std::string{});
        if (!codeOpt || !auth.checkSetupCode(*codeOpt)) {
            loginLimiter.recordFailure(ip);
            emitAuth(events, event::kSetup, event::kDenied, Actor{0, "", ip},
                     {{"reason", "wrong_setup_code"}});
            res->writeStatus("403 Forbidden")
               ->end("Wrong setup code. The server prints it to its log at startup "
                     "(journalctl -u houstonkvm).");
            return;
        }

        auto usernameOpt = jsonValueOr(j, "username", std::string{});
        auto passwordOpt = jsonValueOr(j, "password", std::string{});
        if (!usernameOpt || !passwordOpt) {
            res->writeStatus("400 Bad Request")->end("Invalid JSON");
            return;
        }
        std::string username = *usernameOpt;
        auto password = std::make_shared<std::string>(*passwordOpt);

        if (!isValidUsername(username) ||
            password->size() < 8 || password->size() > 1024) {
            res->writeStatus("400 Bad Request")
               ->end("Invalid username or password (min 8 characters)");
            return;
        }

        auto hash = std::make_shared<std::string>();
        offLoop(hasher, res,
            [&auth, password, hash] {
                try { *hash = auth.hashPassword(*password); }
                catch (const std::exception& e) { std::cerr << "Setup error: " << e.what() << "\n"; }
            },
            [&auth, &db, &loginLimiter, &events, secureCookies, ip, ua, username, hash](Reply<SSL> res) {
                if (!res) return; // nobody to hand the new session to: leave setup open
                if (hash->empty()) {
                    res->writeStatus("500 Internal Server Error")->end("Server error");
                    return;
                }
                auto open = needsSetup(db);
                if (!open) { res->writeStatus("500 Internal Server Error")->end("Server error"); return; }
                if (!*open) { res->writeStatus("409 Conflict")->end("Setup already complete"); return; }

                Stmt ins(db.handle(),
                    "INSERT INTO users (username, password_hash) VALUES (?, ?)");
                ins.bind(1, username).bind(2, *hash);
                if (ins.step() == Stmt::StepResult::Error) {
                    std::cerr << "Setup error [sqlite:" << ins.outcome().sqliteCode << "]: "
                               << ins.outcome().message << "\n";
                    res->writeStatus("500 Internal Server Error")->end("Server error");
                    return;
                }
                int64_t uid = static_cast<int64_t>(sqlite3_last_insert_rowid(db.handle()));
                auth.clearSetupCode();
                loginLimiter.recordSuccess(ip);
                emitAuth(events, event::kSetup, event::kOk, Actor{uid, username, ip});

                DbOutcome outcome;
                auto session = auth.createSession(db, uid, ip, ua, outcome);
                if (!session) {
                    std::cerr << "Setup error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
                    res->writeStatus("500 Internal Server Error")->end("Server error");
                    return;
                }
                res->writeHeader("Set-Cookie", sessionCookieHeader<SSL>(session->token, 604800, secureCookies))
                   ->writeHeader("Content-Type", "application/json")
                   ->end(R"({"ok":true})");
            });
    });
}

template <bool SSL>
static void handleLogin(Auth& auth, Database& db, LoginRateLimiter& loginLimiter,
                         PasswordHasher& hasher, EventBus& events, bool secureCookies,
                         uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    if (!sameOriginOr403(auth.policy, res, req)) return;
    auto ip = clientAddress(auth.policy, res, req);
    auto ua = std::string(req->getHeader("user-agent"));

    readBody(res, 4096, [res, &auth, &db, &loginLimiter, &hasher, &events, secureCookies,
                          ip = std::move(ip), ua = std::move(ua)](std::string& body) mutable {
        auto jOpt = parseJsonOr400(res, body);
        if (!jOpt) return;
        auto& j = *jOpt;

        auto usernameOpt = jsonValueOr(j, "username", std::string{});
        auto passwordOpt = jsonValueOr(j, "password", std::string{});
        if (!usernameOpt || !passwordOpt) {
            res->writeStatus("400 Bad Request")->end("Invalid JSON");
            return;
        }
        std::string username = *usernameOpt;
        auto password = std::make_shared<std::string>(*passwordOpt);

        if (username.empty() || password->empty() ||
            username.size() > 64 || password->size() > 1024) {
            res->writeStatus("400 Bad Request")->end("Missing credentials");
            return;
        }

        Stmt sel(db.handle(),
            "SELECT id, password_hash FROM users WHERE username = ?");
        sel.bind(1, username);
        auto step = sel.step();
        if (step == Stmt::StepResult::Error) {
            std::cerr << "Login error [sqlite:" << sel.outcome().sqliteCode << "]: "
                       << sel.outcome().message << "\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        bool found = step == Stmt::StepResult::Row;
        int64_t uid = found ? sel.column_int64(0) : 0;
        // Empty for a nonexistent username: the worker then verifies against
        // auth.dummyHash() instead, so a missing username still pays the
        // same Argon2 cost as a wrong password. Skipping straight to 401
        // would let response timing reveal which usernames exist.
        auto hash = std::make_shared<std::string>(found ? sel.column_text(1) : "");

        int wait = loginLimiter.beginAttempt(ip);
        if (wait > 0) {
            emitAuth(events, event::kLogin, event::kDenied, Actor{0, username, ip},
                     {{"reason", "locked_out"}});
            res->writeStatus("429 Too Many Requests")
               ->writeHeader("Retry-After", std::to_string(wait))
               ->end("Too many failed login attempts, try again later");
            return;
        }

        auto verified = std::make_shared<bool>(false);
        offLoop(hasher, res,
            [&auth, password, hash, verified] {
                *verified = auth.verifyPassword(*password, hash->empty() ? auth.dummyHash() : *hash);
            },
            [&auth, &db, &loginLimiter, &events, secureCookies, ip, ua, username, found, uid,
             verified](Reply<SSL> res) {
                if (!(found && *verified)) {
                    loginLimiter.recordFailure(ip);
                    emitAuth(events, event::kLogin, event::kDenied, Actor{found ? uid : 0, username, ip},
                             {{"reason", found ? "wrong_password" : "unknown_user"}});
                    if (res) res->writeStatus("401 Unauthorized")->end("Invalid credentials");
                    return;
                }
                loginLimiter.recordSuccess(ip);
                emitAuth(events, event::kLogin, event::kOk, Actor{uid, username, ip});
                if (!res) return;
                DbOutcome outcome;
                auto session = auth.createSession(db, uid, ip, ua, outcome);
                if (!session) {
                    std::cerr << "Login error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
                    res->writeStatus("500 Internal Server Error")->end("Server error");
                    return;
                }
                res->writeHeader("Set-Cookie", sessionCookieHeader<SSL>(session->token, 604800, secureCookies))
                   ->writeHeader("Content-Type", "application/json")
                   ->end(R"({"ok":true})");
            },
            [&loginLimiter, &events, ip, username] {
                loginLimiter.cancelAttempt(ip);
                emitAuth(events, event::kLogin, event::kBusy, Actor{0, username, ip});
            });
    });
}

template <bool SSL>
static void handleLogout(Auth& auth, Database& db, TargetManager& targets, EventBus& events,
                          bool secureCookies,
                          uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    // Logging out releases control, so another page mustn't be able to do it.
    if (!sameOriginOr403(auth.policy, res, req)) return;
    auto token = getCookie(req->getHeader("cookie"), kSessionCookie<SSL>);
    if (!token.empty()) {
        DbOutcome who;
        if (auto u = auth.verifySession(db, token, who))
            emitAuth(events, event::kLogout, event::kOk,
                     Actor{u->userId, u->username, clientAddress(auth.policy, res, req)});
        targets.releaseControlFor(token, "signed_out");
        DbOutcome outcome;
        auth.deleteSession(db, token, outcome);
        if (!outcome.ok) std::cerr << "Logout: " << outcome.message << "\n";
    }
    res->writeHeader("Set-Cookie", sessionCookieHeader<SSL>("", 0, secureCookies))
       ->end();
}

template <bool SSL>
static void handleMe(Auth& auth, Database& db,
                      uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto r = requireRole(auth, db, res, req, Role::Viewer);
    if (!r) return;
    res->writeHeader("Content-Type", "application/json")
       ->end(json{{"ok", true}, {"id", r->userId},
                  {"role", roleToString(r->role)}, {"username", r->username}}.dump());
}

// Longest an API token may be made to last; "never" is asked for with null.
constexpr int kMaxTokenDays = 3650;
constexpr int kDefaultTokenDays = 90;

static json tokenToJson(const Auth::ApiTokenMeta& t) {
    return {{"id", t.id}, {"label", t.label}, {"scope", tokenScopeToString(t.scope)},
            {"created_at", t.createdAt}, {"last_used_at", t.lastUsedAt},
            {"expires_at", t.expiresAt.empty() ? json(nullptr) : json(t.expiresAt)},
            {"expired", t.expired}};
}

template <bool SSL>
static void handleCreateToken(Auth& auth, Database& db, EventBus& events,
                               uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto who = browserAuthenticate(auth, db, res, req);
    if (!who) return;
    int64_t uid = who->userId;
    Role role = who->role;
    Actor actor = actorOf(*who, clientAddress(auth.policy, res, req));

    readBody(res, 4096, [res, &auth, &db, &events, uid, role, actor](std::string& body) mutable {
        std::string label;
        TokenScope scope = TokenScope::Full;
        std::optional<int> days = kDefaultTokenDays;
        if (!body.empty()) {
            auto jOpt = parseJsonOr400(res, body);
            if (!jOpt) return;
            auto labelOpt = jsonValueOr(*jOpt, "label", std::string{});
            auto scopeOpt = jsonValueOr(*jOpt, "scope", std::string{"full"});
            if (!labelOpt || !scopeOpt) {
                res->writeStatus("400 Bad Request")->end("Invalid JSON");
                return;
            }
            label = *labelOpt;
            auto parsed = tokenScopeFromString(*scopeOpt);
            if (!parsed) {
                res->writeStatus("400 Bad Request")->end("scope must be full, read or metrics");
                return;
            }
            scope = *parsed;
            if (auto it = jOpt->find("expires_in_days"); it != jOpt->end()) {
                if (it->is_null()) {
                    days.reset();
                } else if (it->is_number_integer() && it->template get<int64_t>() >= 1 &&
                           it->template get<int64_t>() <= kMaxTokenDays) {
                    days = static_cast<int>(it->template get<int64_t>());
                } else {
                    res->writeStatus("400 Bad Request")
                       ->end("expires_in_days must be 1 to 3650, or null for never");
                    return;
                }
            }
        }
        if (label.size() > 128) {
            res->writeStatus("400 Bad Request")->end("Label too long");
            return;
        }
        if (scope == TokenScope::Metrics && role != Role::Owner) {
            res->writeStatus("403 Forbidden")->end("Only an Owner can make a metrics token");
            return;
        }
        DbOutcome outcome;
        auto info = auth.createApiToken(db, uid, label, scope, days, outcome);
        if (!info) {
            std::cerr << "Create token error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        auto tokens = auth.listApiTokens(db, uid, outcome);
        auto made = std::find_if(tokens.begin(), tokens.end(),
                                 [&](const auto& t) { return t.id == info->id; });
        json meta = made != tokens.end() ? tokenToJson(*made)
                                         : json{{"id", info->id}, {"label", label}};
        emitAuth(events, event::kTokenCreated, event::kOk, actor,
                 {{"token_id", info->id}, {"label", label}, {"scope", meta.value("scope", "")},
                  {"expires_at", meta.value("expires_at", json(nullptr))}});
        meta["token"] = info->token;
        res->writeHeader("Content-Type", "application/json")->end(meta.dump());
    });
}

template <bool SSL>
static void handleListTokens(Auth& auth, Database& db,
                              uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto uidOpt = requireBrowserAuth(auth, db, res, req);
    if (!uidOpt) return;
    DbOutcome outcome;
    auto tokens = auth.listApiTokens(db, *uidOpt, outcome);
    if (!outcome.ok) {
        std::cerr << "List tokens error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
        res->writeStatus("500 Internal Server Error")->end("Server error");
        return;
    }
    json arr = json::array();
    for (auto& t : tokens) arr.push_back(tokenToJson(t));
    res->writeHeader("Content-Type", "application/json")->end(arr.dump());
}

template <bool SSL>
static void handleRevokeToken(Auth& auth, Database& db, EventBus& events,
                               uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto who = browserAuthenticate(auth, db, res, req);
    if (!who) return;
    auto uidOpt = std::optional<int64_t>(who->userId);

    auto tokenId = parseId(req->getParameter(0));
    if (!tokenId) {
        res->writeStatus("400 Bad Request")->end("Invalid token id");
        return;
    }
    DbOutcome outcome;
    auth.revokeApiToken(db, *uidOpt, *tokenId, outcome);
    if (!outcome.ok) {
        std::cerr << "Revoke token error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
        res->writeStatus("500 Internal Server Error")->end("Server error");
        return;
    }
    if (sqlite3_changes(db.handle()) > 0)
        emitAuth(events, event::kTokenRevoked, event::kOk,
                 actorOf(*who, clientAddress(auth.policy, res, req)), {{"token_id", *tokenId}});
    res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
}

template <bool SSL>
static void handleCreateUser(Auth& auth, Database& db, PasswordHasher& hasher, EventBus& events,
                              uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;
    Actor actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));

    readBody(res, 4096, [res, &auth, &db, &hasher, &events, actor](std::string& body) mutable {
        auto jOpt = parseJsonOr400(res, body);
        if (!jOpt) return;
        auto& j = *jOpt;

        auto usernameOpt = jsonValueOr(j, "username", std::string{});
        auto passwordOpt = jsonValueOr(j, "password", std::string{});
        auto roleStrOpt  = jsonValueOr(j, "role", std::string{});
        if (!usernameOpt || !passwordOpt || !roleStrOpt) {
            res->writeStatus("400 Bad Request")->end("Invalid JSON");
            return;
        }
        std::string username = *usernameOpt;
        auto password = std::make_shared<std::string>(*passwordOpt);

        if (!isValidUsername(username) ||
            password->size() < 8 || password->size() > 1024) {
            res->writeStatus("400 Bad Request")
               ->end("Invalid username or password (min 8 characters)");
            return;
        }
        auto role = roleFromString(*roleStrOpt);
        if (!role) {
            res->writeStatus("400 Bad Request")
               ->end("role must be one of: viewer, operator, owner");
            return;
        }

        auto hash = std::make_shared<std::string>();
        offLoop(hasher, res,
            [&auth, password, hash] {
                try { *hash = auth.hashPassword(*password); }
                catch (const std::exception& e) { std::cerr << "Create user error: " << e.what() << "\n"; }
            },
            [&db, &events, actor, username, role = *role, hash](Reply<SSL> res) {
                if (!res) return;
                if (hash->empty()) {
                    res->writeStatus("500 Internal Server Error")->end("Server error");
                    return;
                }
                Stmt ins(db.handle(),
                    "INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)");
                ins.bind(1, username).bind(2, *hash).bind(3, roleToString(role));
                if (ins.step() == Stmt::StepResult::Error) {
                    if (ins.outcome().isConstraintViolation()) {
                        res->writeStatus("409 Conflict")->end("Username already taken");
                    } else {
                        std::cerr << "Create user error [sqlite:" << ins.outcome().sqliteCode << "]: "
                                   << ins.outcome().message << "\n";
                        res->writeStatus("500 Internal Server Error")->end("Server error");
                    }
                    return;
                }
                int64_t uid = static_cast<int64_t>(sqlite3_last_insert_rowid(db.handle()));
                emitAuth(events, event::kUserCreated, event::kOk, actor,
                         {{"user_id", uid}, {"username", username}, {"role", roleToString(role)}});

                res->writeHeader("Content-Type", "application/json")
                   ->end(json{{"id", uid}, {"username", username},
                              {"role", roleToString(role)}}.dump());
            });
    });
}

template <bool SSL>
static void handleListUsers(Auth& auth, Database& db,
                             uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;

    Stmt s(db.handle(),
        "SELECT id, username, role, created_at, COALESCE(last_login_at, '') "
        "FROM users ORDER BY created_at ASC");
    json arr = json::array();
    for (;;) {
        auto step = s.step();
        if (step == Stmt::StepResult::Error) {
            std::cerr << "List users error [sqlite:" << s.outcome().sqliteCode << "]: "
                       << s.outcome().message << "\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        if (step == Stmt::StepResult::Done) break;
        arr.push_back({{"id", s.column_int64(0)},
                        {"username", s.column_text(1)},
                        {"role", s.column_text(2)},
                        {"created_at", s.column_text(3)},
                        {"last_login_at", s.column_text(4)}});
    }
    res->writeHeader("Content-Type", "application/json")->end(arr.dump());
}

template <bool SSL>
static void handleChangeUserRole(Auth& auth, Database& db, EventBus& events,
                                  uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;

    auto targetIdOpt = parseId(req->getParameter(0));
    if (!targetIdOpt) {
        res->writeStatus("400 Bad Request")->end("Invalid user id");
        return;
    }
    int64_t targetId = *targetIdOpt;
    Actor actor = actorOf(*authOpt, clientAddress(auth.policy, res, req));

    readBody(res, 1024, [res, &db, &events, actor, targetId](std::string& body) mutable {
        auto jOpt = parseJsonOr400(res, body);
        if (!jOpt) return;

        auto roleStrOpt = jsonValueOr(*jOpt, "role", std::string{});
        if (!roleStrOpt) {
            res->writeStatus("400 Bad Request")->end("Invalid JSON");
            return;
        }
        auto newRole = roleFromString(*roleStrOpt);
        if (!newRole) {
            res->writeStatus("400 Bad Request")
               ->end("role must be one of: viewer, operator, owner");
            return;
        }

        Stmt cur(db.handle(), "SELECT role, username FROM users WHERE id = ?");
        cur.bind(1, static_cast<sqlite3_int64>(targetId));
        auto curStep = cur.step();
        if (curStep == Stmt::StepResult::Error) {
            std::cerr << "Change role error [sqlite:" << cur.outcome().sqliteCode << "]: "
                       << cur.outcome().message << "\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        if (curStep == Stmt::StepResult::Done) {
            res->writeStatus("404 Not Found")->end("No such user");
            return;
        }
        std::string oldRole = cur.column_text(0), username = cur.column_text(1);
        bool wasOwner = oldRole == "owner";
        if (wasOwner && *newRole != Role::Owner) {
            auto lastOwner = isLastOwner(db, targetId);
            if (!lastOwner) {
                std::cerr << "Change role error: last-owner check failed\n";
                res->writeStatus("500 Internal Server Error")->end("Server error");
                return;
            }
            if (*lastOwner) {
                res->writeStatus("409 Conflict")
                   ->end("Cannot remove the last remaining owner");
                return;
            }
        }
        Stmt upd(db.handle(), "UPDATE users SET role = ? WHERE id = ?");
        upd.bind(1, roleToString(*newRole))
           .bind(2, static_cast<sqlite3_int64>(targetId));
        if (upd.step() == Stmt::StepResult::Error) {
            std::cerr << "Change role error [sqlite:" << upd.outcome().sqliteCode << "]: "
                       << upd.outcome().message << "\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        if (oldRole != roleToString(*newRole))
            emitAuth(events, event::kUserRoleChanged, event::kOk, actor,
                     {{"user_id", targetId}, {"username", username},
                      {"from", oldRole}, {"to", roleToString(*newRole)}});
        res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
    });
}

template <bool SSL>
static void handleDeleteUser(Auth& auth, Database& db, EventBus& events,
                              uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto authOpt = requireBrowserRole(auth, db, res, req, Role::Owner);
    if (!authOpt) return;

    auto targetIdOpt = parseId(req->getParameter(0));
    if (!targetIdOpt) {
        res->writeStatus("400 Bad Request")->end("Invalid user id");
        return;
    }
    int64_t targetId = *targetIdOpt;
    if (targetId == authOpt->userId) {
        res->writeStatus("400 Bad Request")->end("Cannot delete your own account");
        return;
    }

    Stmt cur(db.handle(), "SELECT role, username FROM users WHERE id = ?");
    cur.bind(1, static_cast<sqlite3_int64>(targetId));
    auto curStep = cur.step();
    if (curStep == Stmt::StepResult::Error) {
        std::cerr << "Delete user error [sqlite:" << cur.outcome().sqliteCode << "]: "
                   << cur.outcome().message << "\n";
        res->writeStatus("500 Internal Server Error")->end("Server error");
        return;
    }
    if (curStep == Stmt::StepResult::Done) {
        res->writeStatus("404 Not Found")->end("No such user");
        return;
    }
    std::string role = cur.column_text(0), username = cur.column_text(1);
    if (role == "owner") {
        auto lastOwner = isLastOwner(db, targetId);
        if (!lastOwner) {
            std::cerr << "Delete user error: last-owner check failed\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        if (*lastOwner) {
            res->writeStatus("409 Conflict")
               ->end("Cannot delete the last remaining owner");
            return;
        }
    }

    Stmt del(db.handle(), "DELETE FROM users WHERE id = ?");
    del.bind(1, static_cast<sqlite3_int64>(targetId));
    if (del.step() == Stmt::StepResult::Error) {
        std::cerr << "Delete user error [sqlite:" << del.outcome().sqliteCode << "]: "
                   << del.outcome().message << "\n";
        res->writeStatus("500 Internal Server Error")->end("Server error");
        return;
    }
    emitAuth(events, event::kUserDeleted, event::kOk,
             actorOf(*authOpt, clientAddress(auth.policy, res, req)),
             {{"user_id", targetId}, {"username", username}, {"role", role}});
    res->writeHeader("Content-Type", "application/json")->end(R"({"ok":true})");
}

// Changing your password signs you out everywhere else: whoever might have
// had the old password (or a copied session cookie) loses access at once,
// including control of any target that session was driving. The session
// that made the change stays signed in. API tokens are separate credentials
// and are kept; revoke them under Settings if they may have leaked too.
template <bool SSL>
static void handleChangePassword(Auth& auth, Database& db, TargetManager& targets,
                                  LoginRateLimiter& loginLimiter, PasswordHasher& hasher,
                                  EventBus& events,
                                  uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto who = browserAuthenticate(auth, db, res, req);
    if (!who) return;
    int64_t uid = who->userId;
    std::string sessionToken = who->token;
    auto ip = clientAddress(auth.policy, res, req);
    Actor actor = actorOf(*who, ip);

    readBody(res, 4096, [res, &auth, &db, &targets, &loginLimiter, &hasher, &events, uid, actor,
                          sessionToken = std::move(sessionToken), ip](std::string& body) mutable {
        auto jOpt = parseJsonOr400(res, body);
        if (!jOpt) return;
        auto& j = *jOpt;

        auto currentOpt = jsonValueOr(j, "current_password", std::string{});
        auto newPassOpt = jsonValueOr(j, "new_password", std::string{});
        if (!currentOpt || !newPassOpt) {
            res->writeStatus("400 Bad Request")->end("Invalid field types");
            return;
        }
        auto current = std::make_shared<std::string>(*currentOpt);
        auto newPass = std::make_shared<std::string>(*newPassOpt);
        if (newPass->size() < 8 || newPass->size() > 1024) {
            res->writeStatus("400 Bad Request")->end("New password must be 8 to 1024 characters");
            return;
        }

        Stmt s(db.handle(), "SELECT password_hash FROM users WHERE id = ?");
        s.bind(1, static_cast<sqlite3_int64>(uid));
        auto step = s.step();
        if (step == Stmt::StepResult::Error) {
            std::cerr << "Password change error [sqlite:" << s.outcome().sqliteCode << "]: "
                       << s.outcome().message << "\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return;
        }
        if (step == Stmt::StepResult::Done) {
            res->writeStatus("401 Unauthorized")->end("Not authenticated");
            return;
        }
        auto oldHash = std::make_shared<std::string>(s.column_text(0));

        int wait = loginLimiter.beginAttempt(ip);
        if (wait > 0) {
            res->writeStatus("429 Too Many Requests")
               ->writeHeader("Retry-After", std::to_string(wait))
               ->end("Too many failed attempts, try again later");
            return;
        }

        // Verified and, only if that passes, re-hashed in one trip to the
        // worker, so a wrong guess doesn't also pay for a hash.
        auto verified = std::make_shared<bool>(false);
        auto newHash  = std::make_shared<std::string>();
        offLoop(hasher, res,
            [&auth, current, newPass, oldHash, verified, newHash] {
                *verified = auth.verifyPassword(*current, *oldHash);
                if (!*verified) return;
                try { *newHash = auth.hashPassword(*newPass); }
                catch (const std::exception& e) { std::cerr << "Password change error: " << e.what() << "\n"; }
            },
            [&auth, &db, &targets, &loginLimiter, &events, uid, actor, sessionToken, ip, verified,
             newHash, oldHash](Reply<SSL> res) {
                if (!*verified) {
                    loginLimiter.recordFailure(ip);
                    emitAuth(events, event::kPasswordChanged, event::kDenied, actor,
                             {{"reason", "wrong_current_password"}});
                    if (res) res->writeStatus("401 Unauthorized")->end("Current password is incorrect");
                    return;
                }
                loginLimiter.recordSuccess(ip);
                if (!res) return;
                if (newHash->empty()) {
                    res->writeStatus("500 Internal Server Error")->end("Server error");
                    return;
                }
                // Only if the password is still the one that was just
                // verified: two changes racing through the hasher mustn't
                // let the slower one silently undo the faster.
                Stmt upd(db.handle(),
                    "UPDATE users SET password_hash = ? WHERE id = ? AND password_hash = ?");
                upd.bind(1, *newHash).bind(2, static_cast<sqlite3_int64>(uid)).bind(3, *oldHash);
                if (upd.step() == Stmt::StepResult::Error) {
                    std::cerr << "Password change error [sqlite:" << upd.outcome().sqliteCode << "]: "
                               << upd.outcome().message << "\n";
                    res->writeStatus("500 Internal Server Error")->end("Server error");
                    return;
                }
                if (sqlite3_changes(db.handle()) == 0) {
                    res->writeStatus("409 Conflict")->end("Your password was changed meanwhile; try again");
                    return;
                }

                DbOutcome outcome;
                int signedOut = auth.deleteOtherSessions(db, uid, sessionToken, outcome);
                if (!outcome.ok) {
                    std::cerr << "Password change: signing out other sessions failed [sqlite:"
                              << outcome.sqliteCode << "]: " << outcome.message << "\n";
                    res->writeStatus("500 Internal Server Error")
                       ->end("Password changed, but other sessions could not be signed out");
                    return;
                }
                // A signed-out session may be driving a target; don't wait
                // for the periodic check to take control away from it.
                emitAuth(events, event::kPasswordChanged, event::kOk, actor,
                         {{"signed_out_sessions", signedOut}});
                if (signedOut > 0) targets.revalidateDriversNow();
                res->writeHeader("Content-Type", "application/json")
                   ->end(json{{"ok", true}, {"signed_out_sessions", signedOut}}.dump());
            },
            [&loginLimiter, &events, actor, ip] {
                loginLimiter.cancelAttempt(ip);
                emitAuth(events, event::kPasswordChanged, event::kBusy, actor);
            });
    });
}

template <bool SSL>
void registerAuthRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TargetManager& targets,
                         LoginRateLimiter& loginLimiter, PasswordHasher& hasher, EventBus& events,
                         bool secureCookies) {

app.get("/api/status", [&auth, &db](auto* res, auto* req) {
       handleStatus(auth, db, res, req);
   })

   .post("/api/setup", [&auth, &db, &loginLimiter, &hasher, &events, secureCookies](auto* res, auto* req) {
       handleSetup(auth, db, loginLimiter, hasher, events, secureCookies, res, req);
   })

   .post("/api/login", [&auth, &db, &loginLimiter, &hasher, &events, secureCookies](auto* res, auto* req) {
       handleLogin(auth, db, loginLimiter, hasher, events, secureCookies, res, req);
   })

   .post("/api/logout", [&auth, &db, &targets, &events, secureCookies](auto* res, auto* req) {
       handleLogout(auth, db, targets, events, secureCookies, res, req);
   })

   .get("/api/me", [&auth, &db](auto* res, auto* req) {
       handleMe(auth, db, res, req);
   })

   .post("/api/tokens", [&auth, &db, &events](auto* res, auto* req) {
       handleCreateToken(auth, db, events, res, req);
   })

   .get("/api/tokens", [&auth, &db](auto* res, auto* req) {
       handleListTokens(auth, db, res, req);
   })

   .del("/api/tokens/:id", [&auth, &db, &events](auto* res, auto* req) {
       handleRevokeToken(auth, db, events, res, req);
   })

   .post("/api/users", [&auth, &db, &hasher, &events](auto* res, auto* req) {
       handleCreateUser(auth, db, hasher, events, res, req);
   })

   .get("/api/users", [&auth, &db](auto* res, auto* req) {
       handleListUsers(auth, db, res, req);
   })

   .put("/api/users/:id/role", [&auth, &db, &events](auto* res, auto* req) {
       handleChangeUserRole(auth, db, events, res, req);
   })

   .del("/api/users/:id", [&auth, &db, &events](auto* res, auto* req) {
       handleDeleteUser(auth, db, events, res, req);
   })

   .post("/api/account/password", [&auth, &db, &targets, &loginLimiter, &hasher, &events](auto* res, auto* req) {
       handleChangePassword(auth, db, targets, loginLimiter, hasher, events, res, req);
   });

}

// Plain HTTP and HTTPS run the same routes.
template void registerAuthRoutes<false>(uWS::App&, Auth&, Database&, TargetManager&, LoginRateLimiter&,
                                        PasswordHasher&, EventBus&, bool);
template void registerAuthRoutes<true>(uWS::SSLApp&, Auth&, Database&, TargetManager&, LoginRateLimiter&,
                                       PasswordHasher&, EventBus&, bool);

} // namespace houston_kvm
