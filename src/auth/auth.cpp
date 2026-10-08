#include "auth/auth.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace houston_kvm {

std::optional<Role> roleFromString(const std::string& s) {
    if (s == "viewer")   return Role::Viewer;
    if (s == "operator") return Role::Operator;
    if (s == "owner")    return Role::Owner;
    return std::nullopt;
}

std::string roleToString(Role r) {
    switch (r) {
        case Role::Viewer:   return "viewer";
        case Role::Operator: return "operator";
        case Role::Owner:    return "owner";
    }
    return "viewer"; // unreachable, silences -Wreturn-type
}

std::optional<TokenScope> tokenScopeFromString(const std::string& s) {
    if (s == "full")    return TokenScope::Full;
    if (s == "read")    return TokenScope::Read;
    if (s == "metrics") return TokenScope::Metrics;
    return std::nullopt;
}

std::string tokenScopeToString(TokenScope s) {
    switch (s) {
        case TokenScope::Full:    return "full";
        case TokenScope::Read:    return "read";
        case TokenScope::Metrics: return "metrics";
    }
    return "read";
}

// ── Internal helpers (file-local) ────────────────────────────────────────────

static std::string hexEncode(const uint8_t* data, size_t len) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out(len * 2, '\0');
    for (size_t i = 0; i < len; ++i) {
        out[2 * i]     = HEX[data[i] >> 4];
        out[2 * i + 1] = HEX[data[i] & 0x0F];
    }
    return out;
}

static bool hexDecode(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    out.resize(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned int byte = 0;
        const char* start = hex.data() + 2 * i;
        const char* end   = start + 2;
        auto [ptr, ec] = std::from_chars(start, end, byte, 16);
        if (ec != std::errc{} || ptr != end) return false;
        out[i] = static_cast<uint8_t>(byte);
    }
    return true;
}

static std::string sha256Hex(const uint8_t* data, size_t len) {
    uint8_t digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, data, len);
    return hexEncode(digest, sizeof(digest));
}

Auth::Auth() {
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium: sodium_init() failed");
}

const std::string& Auth::issueSetupCode() {
    // 12 characters from an alphabet without look-alikes (0/O, 1/I/L), shown
    // as XXXX-XXXX-XXXX: ~59 bits, read off a console and typed by hand.
    static constexpr char kAlphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
    setupCode_.clear();
    for (int i = 0; i < 12; ++i) {
        if (i > 0 && i % 4 == 0) setupCode_ += '-';
        setupCode_ += kAlphabet[randombytes_uniform(sizeof(kAlphabet) - 1)];
    }
    return setupCode_;
}

bool Auth::checkSetupCode(const std::string& code) const {
    if (setupCode_.empty()) return false;
    // Forgive case and where the dashes and spaces went.
    auto normalize = [](const std::string& s) {
        std::string out;
        for (char c : s)
            if (c != '-' && c != ' ') out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    };
    auto want = normalize(setupCode_), got = normalize(code);
    return got.size() == want.size() && sodium_memcmp(got.data(), want.data(), want.size()) == 0;
}

std::string Auth::hashPassword(const std::string& password) const {
    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(
            hash,
            password.c_str(), password.size(),
            crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
        throw std::runtime_error("crypto_pwhash_str: insufficient memory");
    return std::string(hash);
}

bool Auth::verifyPassword(const std::string& password,
                           const std::string& phcHash) const {
    return crypto_pwhash_str_verify(
               phcHash.c_str(),
               password.c_str(), password.size()) == 0;
}

const std::string& Auth::dummyHash() const {
    // Magic-static init is thread-safe and runs at most once; the constant
    // string is arbitrary — it's never checked against a real password.
    static const std::string hash = hashPassword("houstonkvm-timing-equalization-dummy");
    return hash;
}

std::optional<Auth::SessionInfo> Auth::createSession(Database& db,
                                       int64_t userId,
                                       const std::string& ip,
                                       const std::string& userAgent,
                                       DbOutcome& outcome,
                                       int ttlSeconds) const {
    // Sessions are only ever created here, so clearing out expired ones at
    // the same time keeps the table bounded by the sign-ins of the last
    // session lifetime. The expires_at index makes it cheap.
    DbOutcome purged;
    purgeExpiredSessions(db, purged);
    if (!purged.ok) std::cerr << "Session purge failed: " << purged.message << "\n";

    uint8_t raw[32];
    randombytes_buf(raw, sizeof(raw));

    SessionInfo info;
    info.token     = hexEncode(raw, sizeof(raw));
    info.tokenHash = sha256Hex(raw, sizeof(raw));
    // SQLite datetime modifier: "+N seconds"
    std::string modifier = "+" + std::to_string(ttlSeconds) + " seconds";

    Stmt ins(db.handle(),
        "INSERT INTO sessions (token_hash, user_id, expires_at, user_agent, ip) "
        "VALUES (?, ?, datetime('now', ?), ?, ?)");
    ins.bind(1, info.tokenHash)
       .bind(2, static_cast<sqlite3_int64>(userId))
       .bind(3, modifier)
       .bind(4, userAgent)
       .bind(5, ip);
    if (ins.step() == Stmt::StepResult::Error) { outcome = ins.outcome(); return std::nullopt; }

    Stmt upd(db.handle(),
        "UPDATE users SET last_login_at = datetime('now') WHERE id = ?");
    upd.bind(1, static_cast<sqlite3_int64>(userId));
    if (upd.step() == Stmt::StepResult::Error) { outcome = upd.outcome(); return std::nullopt; }

    return info;
}

std::optional<Auth::AuthenticatedUser> Auth::verifySession(Database& db,
                                            const std::string& token,
                                            DbOutcome& outcome) const {
    std::vector<uint8_t> raw;
    if (!hexDecode(token, raw) || raw.size() != 32)
        return std::nullopt;

    std::string hash = sha256Hex(raw.data(), raw.size());
    Stmt stmt(db.handle(),
        "SELECT sessions.user_id, users.role, users.username FROM sessions "
        "JOIN users ON users.id = sessions.user_id "
        "WHERE sessions.token_hash = ? AND sessions.expires_at > datetime('now')");
    stmt.bind(1, hash);
    auto step = stmt.step();
    if (step == Stmt::StepResult::Error) { outcome = stmt.outcome(); return std::nullopt; }
    if (step == Stmt::StepResult::Done)  return std::nullopt;
    Role role = roleFromString(stmt.column_text(1)).value_or(Role::Viewer);
    return AuthenticatedUser{stmt.column_int64(0), role, stmt.column_text(2)};
}

void Auth::deleteSession(Database& db, const std::string& token, DbOutcome& outcome) const {
    std::vector<uint8_t> raw;
    if (!hexDecode(token, raw) || raw.size() != 32) return;
    std::string hash = sha256Hex(raw.data(), raw.size());

    Stmt stmt(db.handle(), "DELETE FROM sessions WHERE token_hash = ?");
    stmt.bind(1, hash);
    if (stmt.step() == Stmt::StepResult::Error) outcome = stmt.outcome();
}

int Auth::deleteOtherSessions(Database& db, int64_t userId, const std::string& keepToken,
                              DbOutcome& outcome) const {
    std::string keepHash;
    std::vector<uint8_t> raw;
    if (hexDecode(keepToken, raw) && raw.size() == 32) keepHash = sha256Hex(raw.data(), raw.size());

    Stmt stmt(db.handle(), "DELETE FROM sessions WHERE user_id = ? AND token_hash != ?");
    stmt.bind(1, static_cast<sqlite3_int64>(userId)).bind(2, keepHash);
    if (stmt.step() == Stmt::StepResult::Error) { outcome = stmt.outcome(); return 0; }
    return sqlite3_changes(db.handle());
}

int Auth::purgeExpiredSessions(Database& db, DbOutcome& outcome) const {
    Stmt stmt(db.handle(), "DELETE FROM sessions WHERE expires_at <= datetime('now')");
    if (stmt.step() == Stmt::StepResult::Error) { outcome = stmt.outcome(); return 0; }
    return sqlite3_changes(db.handle());
}

std::optional<Auth::ApiTokenInfo> Auth::createApiToken(Database& db, int64_t userId,
                                         const std::string& label,
                                         TokenScope scope,
                                         std::optional<int> expiresInDays,
                                         DbOutcome& outcome) const {
    uint8_t raw[32];
    randombytes_buf(raw, sizeof(raw));

    ApiTokenInfo info;
    info.token     = hexEncode(raw, sizeof(raw));
    info.tokenHash = sha256Hex(raw, sizeof(raw));

    Stmt ins(db.handle(),
        "INSERT INTO api_tokens (token_hash, user_id, label, scope, expires_at) "
        "VALUES (?, ?, ?, ?, CASE WHEN ? IS NULL THEN NULL "
        "ELSE datetime('now', '+' || ? || ' days') END)");
    ins.bind(1, info.tokenHash)
       .bind(2, static_cast<sqlite3_int64>(userId))
       .bind(3, label)
       .bind(4, tokenScopeToString(scope));
    if (expiresInDays) {
        ins.bind(5, static_cast<sqlite3_int64>(*expiresInDays))
           .bind(6, static_cast<sqlite3_int64>(*expiresInDays));
    } else {
        ins.bindNull(5).bindNull(6);
    }
    if (ins.step() == Stmt::StepResult::Error) { outcome = ins.outcome(); return std::nullopt; }

    info.id = static_cast<int64_t>(sqlite3_last_insert_rowid(db.handle()));
    return info;
}

std::optional<Auth::AuthenticatedUser> Auth::verifyApiToken(Database& db,
                                             const std::string& token,
                                             DbOutcome& outcome) const {
    std::vector<uint8_t> raw;
    if (!hexDecode(token, raw) || raw.size() != 32)
        return std::nullopt;

    std::string hash = sha256Hex(raw.data(), raw.size());
    Stmt stmt(db.handle(),
        "SELECT api_tokens.user_id, users.role, users.username, api_tokens.scope "
        "FROM api_tokens JOIN users ON users.id = api_tokens.user_id "
        "WHERE api_tokens.token_hash = ? "
        "AND (api_tokens.expires_at IS NULL OR api_tokens.expires_at > datetime('now'))");
    stmt.bind(1, hash);
    auto step = stmt.step();
    if (step == Stmt::StepResult::Error) { outcome = stmt.outcome(); return std::nullopt; }
    if (step == Stmt::StepResult::Done)  return std::nullopt;
    int64_t userId = static_cast<int64_t>(stmt.column_int64(0));
    Role role = roleFromString(stmt.column_text(1)).value_or(Role::Viewer);
    std::string username = stmt.column_text(2);
    // An unknown scope (a newer server's, after a downgrade) grants nothing
    // more than reading.
    TokenScope scope = tokenScopeFromString(stmt.column_text(3)).value_or(TokenScope::Read);
    if (scope == TokenScope::Read) role = std::min(role, Role::Viewer);
    Stmt upd(db.handle(),
        "UPDATE api_tokens SET last_used_at = datetime('now') "
        "WHERE token_hash = ? "
        "AND (last_used_at IS NULL OR last_used_at < datetime('now', '-60 seconds'))");
    upd.bind(1, hash);
    (void)upd.step();

    return AuthenticatedUser{userId, role, username, scope};
}

std::vector<Auth::ApiTokenMeta> Auth::listApiTokens(Database& db, int64_t userId, DbOutcome& outcome) const {
    std::vector<ApiTokenMeta> out;
    Stmt stmt(db.handle(),
        "SELECT id, label, created_at, COALESCE(last_used_at, ''), scope, "
        "COALESCE(expires_at, ''), expires_at IS NOT NULL AND expires_at <= datetime('now') "
        "FROM api_tokens WHERE user_id = ? ORDER BY created_at DESC");
    stmt.bind(1, static_cast<sqlite3_int64>(userId));
    for (;;) {
        auto step = stmt.step();
        if (step == Stmt::StepResult::Error) { outcome = stmt.outcome(); return {}; }
        if (step == Stmt::StepResult::Done) break;
        out.push_back(ApiTokenMeta{
            stmt.column_int64(0), stmt.column_text(1),
            stmt.column_text(2),  stmt.column_text(3),
            tokenScopeFromString(stmt.column_text(4)).value_or(TokenScope::Read),
            stmt.column_text(5),  stmt.column_int(6) != 0});
    }
    return out;
}

void Auth::revokeApiToken(Database& db, int64_t userId, int64_t tokenId, DbOutcome& outcome) const {
    Stmt stmt(db.handle(), "DELETE FROM api_tokens WHERE id = ? AND user_id = ?");
    stmt.bind(1, static_cast<sqlite3_int64>(tokenId))
        .bind(2, static_cast<sqlite3_int64>(userId));
    if (stmt.step() == Stmt::StepResult::Error) outcome = stmt.outcome();
}

}
