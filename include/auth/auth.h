#pragma once

#include "core/database.h"

#include <sodium.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace houston_kvm {

enum class Role { Viewer = 0, Operator = 1, Owner = 2 };
std::optional<Role> roleFromString(const std::string& s);
std::string roleToString(Role r);

// What an API token may do, at most; never more than its account's role.
//  - Full:    everything the account may do.
//  - Read:    what a Viewer may: watch and read status, change nothing.
//  - Metrics: GET /metrics only, for a monitoring system to hold. Only an
//             Owner may make one, since the metrics describe the whole server.
// Browser sessions are always Full.
enum class TokenScope { Full, Read, Metrics };
std::optional<TokenScope> tokenScopeFromString(const std::string& s);
std::string tokenScopeToString(TokenScope s);

// Set once from the command line before the server starts; read by
// sameOriginOr403() and clientAddress() in core/http_common.h.
struct RequestPolicy {
    // Browser origins ("https://kvm.example.com") allowed to send
    // cookie-authenticated requests besides the one the Host header names.
    // Needed only behind a proxy that rewrites Host.
    std::vector<std::string> allowedOrigins;
    // Peer addresses whose X-Forwarded-For is believed when deciding which
    // client a request came from (login rate limiting, session records).
    std::vector<std::string> trustedProxies;
};

class Auth {
public:
    // Initialises libsodium.  Throws std::runtime_error on failure.
    Auth();

    RequestPolicy policy;

    // While no account exists, POST /api/setup must carry this one-time
    // code, which the server prints to its log at startup. Without it, the
    // first visitor to reach a freshly installed server would become its
    // Owner. Empty once setup has happened.
    const std::string& issueSetupCode();
    bool checkSetupCode(const std::string& code) const;
    void clearSetupCode() { setupCode_.clear(); }
    std::string hashPassword(const std::string& password) const;

    bool verifyPassword(const std::string& password,
                        const std::string& phcHash) const;

    // A valid Argon2 hash not tied to any real user, computed once and
    // cached. Verify a login's password against this when the username
    // doesn't exist, so the "no such user" and "wrong password" paths cost
    // the same wall-clock time — otherwise skipping the hash verify on a
    // missing username lets an attacker enumerate valid usernames by
    // response timing.
    const std::string& dummyHash() const;
    struct SessionInfo {
        std::string token;      ///< 64-char hex token to send to the client
        std::string tokenHash;  ///< SHA-256 hex of raw token bytes, stored in DB
    };

    struct AuthenticatedUser {
        int64_t userId;
        Role    role;        ///< already limited by the token's scope
        std::string username;
        TokenScope scope = TokenScope::Full;
    };

    std::optional<SessionInfo> createSession(Database& db,
                                              int64_t userId,
                                              const std::string& ip,
                                              const std::string& userAgent,
                                              DbOutcome& outcome,
                                              int ttlSeconds = 86400 * 7) const;
    std::optional<AuthenticatedUser> verifySession(Database& db,
                                                    const std::string& token,
                                                    DbOutcome& outcome) const;
    
    void deleteSession(Database& db, const std::string& token, DbOutcome& outcome) const;

    // Signs the user out everywhere except the session `keepToken` (after a
    // password change, so a stolen cookie stops working). API tokens are
    // separate credentials the user manages themselves and are kept.
    // Returns how many sessions were removed.
    int deleteOtherSessions(Database& db, int64_t userId, const std::string& keepToken,
                            DbOutcome& outcome) const;

    // Removes sessions past their expiry. verifySession() already refuses
    // them; this only keeps the table from growing forever. Returns how many
    // were removed.
    int purgeExpiredSessions(Database& db, DbOutcome& outcome) const;

    struct ApiTokenInfo {
        std::string token;      ///< 64-char hex token, returned to the caller once
        std::string tokenHash;  ///< SHA-256 hex of raw token bytes, stored in DB
        int64_t     id;         ///< api_tokens.id, for later revocation
    };

    struct ApiTokenMeta {
        int64_t     id;
        std::string label;
        std::string createdAt;
        std::string lastUsedAt; ///< empty string if never used
        TokenScope  scope;
        std::string expiresAt;  ///< empty string if it never expires
        bool        expired;
    };

    // `expiresInDays` empty: never expires.
    std::optional<ApiTokenInfo> createApiToken(Database& db, int64_t userId,
                                                const std::string& label,
                                                TokenScope scope,
                                                std::optional<int> expiresInDays,
                                                DbOutcome& outcome) const;

    // Nothing for an unknown or expired token. The role returned is the
    // account's, limited by the token's scope.
    std::optional<AuthenticatedUser> verifyApiToken(Database& db,
                                                      const std::string& token,
                                                      DbOutcome& outcome) const;
    
    std::vector<ApiTokenMeta> listApiTokens(Database& db, int64_t userId, DbOutcome& outcome) const;
    void revokeApiToken(Database& db, int64_t userId, int64_t tokenId, DbOutcome& outcome) const;

private:
    std::string setupCode_;
};

}
