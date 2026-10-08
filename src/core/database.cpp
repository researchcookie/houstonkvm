#include <sqlite3.h>
#include "core/database.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>


namespace houston_kvm {

namespace fs = std::filesystem;

Database::Database(const std::string& path) {
    int rc = sqlite3_open_v2(
        path.c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        openError_ = db_ ? sqlite3_errmsg(db_) : "Failed to open database";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return;
    }

    DbOutcome outcome;
    bool pragmasOk = execute("PRAGMA journal_mode=WAL;", &outcome)
        && execute("PRAGMA synchronous=NORMAL;", &outcome)
        && execute("PRAGMA foreign_keys=ON;", &outcome)
        && execute("PRAGMA busy_timeout=5000;", &outcome);
    if (!pragmasOk) {
        openError_ = outcome.message;
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    isOpen_ = true;
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::mustExecute(const std::string& sql) {
    DbOutcome outcome;
    if (!execute(sql, &outcome)) {
        std::cerr << "Fatal: schema setup failed [sqlite:" << outcome.sqliteCode << "]: "
                   << outcome.message << "\n";
        std::exit(1);
    }
}

void Database::init() {
    mustExecute(R"(
        CREATE TABLE IF NOT EXISTS users (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            username      TEXT    NOT NULL UNIQUE,
            password_hash TEXT    NOT NULL,
            created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
            last_login_at TEXT
        );
    )");
    mustExecute(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            token_hash TEXT    PRIMARY KEY,
            user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_at TEXT    NOT NULL DEFAULT (datetime('now')),
            expires_at TEXT    NOT NULL,
            user_agent TEXT,
            ip         TEXT
        );
    )");
    mustExecute(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_users_username "
        "ON users(username);");
    mustExecute(
        "CREATE INDEX IF NOT EXISTS idx_sessions_user_id "
        "ON sessions(user_id);");
    mustExecute(
        "CREATE INDEX IF NOT EXISTS idx_sessions_expires_at "
        "ON sessions(expires_at);");
    mustExecute(R"(
        CREATE TABLE IF NOT EXISTS api_tokens (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            token_hash    TEXT    NOT NULL UNIQUE,
            user_id       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            label         TEXT    NOT NULL DEFAULT '',
            created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
            last_used_at  TEXT
        );
    )");
    mustExecute(
        "CREATE INDEX IF NOT EXISTS idx_api_tokens_user_id "
        "ON api_tokens(user_id);");
    mustExecute(R"(
        CREATE TABLE IF NOT EXISTS user_settings (
            user_id              INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
            theme                TEXT    NOT NULL DEFAULT 'dark',
            default_stream_mode  TEXT    NOT NULL DEFAULT 'mjpeg',
            webrtc_bitrate_kbps  INTEGER,
            updated_at           TEXT    NOT NULL DEFAULT (datetime('now'))
        );
    )");
    // Server-wide stream/hardware settings from before targets: one row
    // (CHECK(id=1)), which migration 5 copies into `targets`. Nothing reads
    // or writes it after that.
    mustExecute(R"(
        CREATE TABLE IF NOT EXISTS server_settings (
            id                   INTEGER PRIMARY KEY CHECK (id = 1),
            v4l2_device          TEXT    NOT NULL DEFAULT '/dev/video0',
            capture_width        INTEGER NOT NULL DEFAULT 1280,
            capture_height       INTEGER NOT NULL DEFAULT 720,
            capture_fps          INTEGER NOT NULL DEFAULT 30,
            qmp_socket           TEXT    NOT NULL DEFAULT '',
            serial_device        TEXT    NOT NULL DEFAULT '',
            serial_baud          INTEGER NOT NULL DEFAULT 9600,
            webrtc_bitrate_kbps  INTEGER NOT NULL DEFAULT 4000,
            updated_at           TEXT    NOT NULL DEFAULT (datetime('now'))
        );
    )");
    mustExecute("INSERT OR IGNORE INTO server_settings (id) VALUES (1);");
}

void Database::migrate() {
    // Scoped so the prepared statement finalizes before any migration below
    // runs — a schema-changing statement (CREATE/DROP TABLE, used by the
    // version-4 migration) on this same connection fails with "database
    // table is locked" while another statement from this connection is
    // still open, even a read-only PRAGMA one.
    int version = 0;
    {
        Stmt v(db_, "PRAGMA user_version");
        if (v.step() == Stmt::StepResult::Row) version = v.column_int(0);
    }
    if (version < 1) {
        mustExecute("ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'owner'");
        mustExecute("PRAGMA user_version = 1");
    }
    if (version < 2) {
        mustExecute("ALTER TABLE server_settings ADD COLUMN mouse_relative "
                     "INTEGER NOT NULL DEFAULT 0");
        mustExecute("PRAGMA user_version = 2");
    }
    if (version < 3) {
        mustExecute("ALTER TABLE server_settings ADD COLUMN mouse_relative_sensitivity "
                     "REAL NOT NULL DEFAULT 1.0");
        mustExecute("PRAGMA user_version = 3");
    }
    if (version < 4) {
        // Relative mouse mode removed entirely — see Ch9329Inject. Absolute
        // mode's HID reports are spec-guaranteed to scale linearly to the
        // target's screen; relative reports are raw, uncalibrated counts
        // subject to the target OS's own (nonlinear, mutable) pointer
        // acceleration, with no feedback channel to correct for it. Not
        // worth keeping as a fallback.
        //
        // `ALTER TABLE ... DROP COLUMN` needs SQLite >= 3.35 (2021); can't
        // assume that's what's installed (e.g. RHEL/CentOS commonly ships
        // 3.34), so rebuild the table instead — standard portable technique
        // for a column drop pre-3.35, safe here since server_settings is
        // always exactly one row.
        mustExecute(R"(
            CREATE TABLE server_settings_new (
                id                   INTEGER PRIMARY KEY CHECK (id = 1),
                v4l2_device          TEXT    NOT NULL DEFAULT '/dev/video0',
                capture_width        INTEGER NOT NULL DEFAULT 1280,
                capture_height       INTEGER NOT NULL DEFAULT 720,
                capture_fps          INTEGER NOT NULL DEFAULT 30,
                qmp_socket           TEXT    NOT NULL DEFAULT '',
                serial_device        TEXT    NOT NULL DEFAULT '',
                serial_baud          INTEGER NOT NULL DEFAULT 9600,
                webrtc_bitrate_kbps  INTEGER NOT NULL DEFAULT 4000,
                updated_at           TEXT    NOT NULL DEFAULT (datetime('now'))
            );
        )");
        mustExecute(R"(
            INSERT INTO server_settings_new
                (id, v4l2_device, capture_width, capture_height, capture_fps,
                 qmp_socket, serial_device, serial_baud, webrtc_bitrate_kbps, updated_at)
            SELECT id, v4l2_device, capture_width, capture_height, capture_fps,
                   qmp_socket, serial_device, serial_baud, webrtc_bitrate_kbps, updated_at
            FROM server_settings;
        )");
        mustExecute("DROP TABLE server_settings");
        mustExecute("ALTER TABLE server_settings_new RENAME TO server_settings");
        mustExecute("PRAGMA user_version = 4");
    }
    if (version < 5) {
        // Multi-target: the single server_settings row becomes one row in
        // `targets`, marked default so every existing unscoped /api/* route
        // and client keeps addressing the same machine it always did.
        // server_settings itself is left in place (unread from here on) so
        // a downgrade still finds its old config rather than a missing table.
        //
        // The unique indexes are what stop two targets from claiming the
        // same physical device: two open()s of one serial port succeed and
        // would interleave frames to two CH9329s; a second capture open
        // fails only at STREAMON. Empty means "unset", not "the same".
        // idx_targets_gadget: the local USB-HID-gadget fallback (no QMP
        // socket, no serial device) drives /dev/hidg0 and /dev/hidg1 — one
        // UDC, so at most one target can use it.
        //
        // One transaction so a failure can't leave a half-built schema that
        // the next boot's CREATE TABLE would then trip over.
        mustExecute("BEGIN IMMEDIATE");
        mustExecute(R"(
            CREATE TABLE targets (
                id                   INTEGER PRIMARY KEY AUTOINCREMENT,
                name                 TEXT    NOT NULL UNIQUE COLLATE NOCASE,
                description          TEXT    NOT NULL DEFAULT '',
                group_name           TEXT    NOT NULL DEFAULT '',
                enabled              INTEGER NOT NULL DEFAULT 1,
                is_default           INTEGER NOT NULL DEFAULT 0,
                v4l2_device          TEXT    NOT NULL DEFAULT '',
                capture_width        INTEGER NOT NULL DEFAULT 1280,
                capture_height       INTEGER NOT NULL DEFAULT 720,
                capture_fps          INTEGER NOT NULL DEFAULT 30,
                qmp_socket           TEXT    NOT NULL DEFAULT '',
                serial_device        TEXT    NOT NULL DEFAULT '',
                serial_baud          INTEGER NOT NULL DEFAULT 9600,
                webrtc_bitrate_kbps  INTEGER NOT NULL DEFAULT 4000,
                created_at           TEXT    NOT NULL DEFAULT (datetime('now')),
                updated_at           TEXT    NOT NULL DEFAULT (datetime('now'))
            );
        )");
        mustExecute("CREATE UNIQUE INDEX idx_targets_default ON targets(is_default) WHERE is_default = 1");
        mustExecute("CREATE UNIQUE INDEX idx_targets_v4l2 ON targets(v4l2_device) WHERE v4l2_device != ''");
        mustExecute("CREATE UNIQUE INDEX idx_targets_serial ON targets(serial_device) WHERE serial_device != ''");
        mustExecute("CREATE UNIQUE INDEX idx_targets_qmp ON targets(qmp_socket) WHERE qmp_socket != ''");
        mustExecute("CREATE UNIQUE INDEX idx_targets_gadget ON targets((1)) "
                     "WHERE qmp_socket = '' AND serial_device = ''");
        mustExecute("CREATE INDEX idx_targets_group ON targets(group_name COLLATE NOCASE)");
        mustExecute(R"(
            CREATE TABLE target_tags (
                target_id INTEGER NOT NULL REFERENCES targets(id) ON DELETE CASCADE,
                tag       TEXT    NOT NULL,
                PRIMARY KEY (target_id, tag)
            );
        )");
        mustExecute("CREATE INDEX idx_target_tags_tag ON target_tags(tag)");
        mustExecute(R"(
            INSERT INTO targets
                (name, is_default, v4l2_device, capture_width, capture_height, capture_fps,
                 qmp_socket, serial_device, serial_baud, webrtc_bitrate_kbps)
            SELECT 'Default target', 1, v4l2_device, capture_width, capture_height, capture_fps,
                   qmp_socket, serial_device, serial_baud, webrtc_bitrate_kbps
            FROM server_settings WHERE id = 1;
        )");
        mustExecute("PRAGMA user_version = 5");
        mustExecute("COMMIT");
    }
    if (version < 6) {
        // The audit log (see core/audit_log.h). No foreign keys to users or
        // targets on purpose: the record of what an account or a target did
        // must outlive the account or target. Timestamps are UTC ISO 8601
        // with milliseconds, as the API returns them.
        mustExecute("BEGIN");
        mustExecute(R"(
            CREATE TABLE audit_events (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                at          TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                type        TEXT    NOT NULL,
                outcome     TEXT    NOT NULL,
                user_id     INTEGER,
                username    TEXT    NOT NULL DEFAULT '',
                ip          TEXT    NOT NULL DEFAULT '',
                via         TEXT    NOT NULL DEFAULT '',
                target_id   INTEGER,
                target_name TEXT    NOT NULL DEFAULT '',
                detail      TEXT    NOT NULL DEFAULT '{}'
            );
        )");
        mustExecute("CREATE INDEX idx_audit_events_at ON audit_events(at)");
        mustExecute("CREATE INDEX idx_audit_events_type ON audit_events(type, id)");
        mustExecute("CREATE INDEX idx_audit_events_user ON audit_events(user_id, id)");
        mustExecute("CREATE INDEX idx_audit_events_target ON audit_events(target_id, id)");
        mustExecute(R"(
            CREATE TABLE audit_control_sessions (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                target_id   INTEGER NOT NULL,
                target_name TEXT    NOT NULL DEFAULT '',
                user_id     INTEGER,
                username    TEXT    NOT NULL DEFAULT '',
                ip          TEXT    NOT NULL DEFAULT '',
                via         TEXT    NOT NULL DEFAULT '',
                started_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                ended_at    TEXT,
                end_reason  TEXT,
                key_count   INTEGER NOT NULL DEFAULT 0,   -- key presses
                mouse_count INTEGER NOT NULL DEFAULT 0    -- mouse moves, clicks, scrolls
            );
        )");
        mustExecute("CREATE INDEX idx_audit_sessions_started ON audit_control_sessions(started_at)");
        mustExecute("CREATE INDEX idx_audit_sessions_target ON audit_control_sessions(target_id, id)");
        mustExecute("CREATE INDEX idx_audit_sessions_user ON audit_control_sessions(user_id, id)");
        // Deliberately no record of WHICH keys were pressed or where the
        // mouse went: that would keep every password typed into a target.
        mustExecute("PRAGMA user_version = 6");
        mustExecute("COMMIT");
    }
    if (version < 7) {
        // Where each target's sound comes from (video/audio_capture.h).
        // Existing targets get "auto": the capture dongle's own sound card,
        // if it has one. Like the other device columns, no two targets may
        // name the same sound device; "auto" and "" (off) aren't devices.
        mustExecute("BEGIN");
        mustExecute("ALTER TABLE targets ADD COLUMN audio_device TEXT NOT NULL DEFAULT 'auto'");
        mustExecute("CREATE UNIQUE INDEX idx_targets_audio ON targets(audio_device) "
                     "WHERE audio_device NOT IN ('', 'auto')");
        mustExecute("PRAGMA user_version = 7");
        mustExecute("COMMIT");
    }
    if (version < 8) {
        // STUN/TURN servers for low-latency video, set by an Owner. None by
        // default.
        mustExecute("ALTER TABLE server_settings ADD COLUMN ice_servers TEXT NOT NULL DEFAULT '[]'");
        mustExecute("PRAGMA user_version = 8");
    }
    if (version < 9) {
        // HTTPS, which an Owner turns on (tls/tls_manager.h). Off by default,
        // including on upgrade. The certificate itself is kept in files.
        mustExecute("BEGIN");
        mustExecute("ALTER TABLE server_settings ADD COLUMN tls_enabled INTEGER NOT NULL DEFAULT 0");
        mustExecute("ALTER TABLE server_settings ADD COLUMN https_port INTEGER NOT NULL DEFAULT 8443");
        mustExecute("ALTER TABLE server_settings ADD COLUMN http_redirect INTEGER NOT NULL DEFAULT 1");
        mustExecute("ALTER TABLE server_settings ADD COLUMN hsts INTEGER NOT NULL DEFAULT 0");
        mustExecute("ALTER TABLE server_settings ADD COLUMN tls_auto_renew INTEGER NOT NULL DEFAULT 1");
        mustExecute("PRAGMA user_version = 9");
        mustExecute("COMMIT");
    }
    if (version < 10) {
        // API tokens get a scope (auth/auth.h, TokenScope) and an expiry.
        // Tokens made before this keep working as they did: the account's
        // full role, no expiry. NULL expires_at means never.
        mustExecute("BEGIN");
        mustExecute("ALTER TABLE api_tokens ADD COLUMN scope TEXT NOT NULL DEFAULT 'full'");
        mustExecute("ALTER TABLE api_tokens ADD COLUMN expires_at TEXT");
        mustExecute("PRAGMA user_version = 10");
        mustExecute("COMMIT");
    }
}

namespace {

constexpr const char* kTargetColumns =
    "t.id, t.name, t.description, t.group_name, t.enabled, t.is_default, "
    "t.v4l2_device, t.capture_width, t.capture_height, t.capture_fps, "
    "t.qmp_socket, t.serial_device, t.serial_baud, t.webrtc_bitrate_kbps, t.audio_device";

Database::Target rowToTarget(const Stmt& s) {
    Database::Target t;
    t.id          = s.column_int64(0);
    t.name        = s.column_text(1);
    t.description = s.column_text(2);
    t.group       = s.column_text(3);
    t.enabled     = s.column_int(4) != 0;
    t.isDefault   = s.column_int(5) != 0;
    t.settings.v4l2Device        = s.column_text(6);
    t.settings.captureWidth      = static_cast<uint32_t>(s.column_int(7));
    t.settings.captureHeight     = static_cast<uint32_t>(s.column_int(8));
    t.settings.captureFps        = static_cast<uint32_t>(s.column_int(9));
    t.settings.qmpSocket         = s.column_text(10);
    t.settings.serialDevice      = s.column_text(11);
    t.settings.serialBaud        = s.column_int(12);
    t.settings.webrtcBitrateKbps = static_cast<uint32_t>(s.column_int(13));
    t.settings.audioDevice       = s.column_text(14);
    return t;
}

// Escapes LIKE wildcards so a search for "100%" or "rack_3" matches
// literally (queries use ESCAPE '\').
std::string likeContains(const std::string& term) {
    std::string out = "%";
    for (char c : term) {
        if (c == '\\' || c == '%' || c == '_') out += '\\';
        out += c;
    }
    out += '%';
    return out;
}

void reportFailure(const char* what, const Stmt& s) {
    std::cerr << "Database: " << what << " failed [sqlite:" << s.outcome().sqliteCode
               << "]: " << s.outcome().message << "\n";
}

void copyOutcome(const Stmt& s, DbOutcome* outcome) {
    if (outcome) *outcome = s.outcome();
}

} // namespace

bool Database::beginTx(DbOutcome* outcome) { return execute("BEGIN IMMEDIATE", outcome); }

bool Database::endTx(bool commit) { return execute(commit ? "COMMIT" : "ROLLBACK"); }

void Database::loadTags(std::vector<Target>& targets) {
    if (targets.empty()) return;
    Stmt s(db_, "SELECT target_id, tag FROM target_tags ORDER BY tag");
    for (;;) {
        auto step = s.step();
        if (step == Stmt::StepResult::Error) { reportFailure("loadTags", s); return; }
        if (step == Stmt::StepResult::Done) return;
        int64_t id = s.column_int64(0);
        for (auto& t : targets)
            if (t.id == id) { t.tags.push_back(s.column_text(1)); break; }
    }
}

std::vector<Database::Target> Database::listTargets(const TargetQuery& query) {
    std::string sql = std::string("SELECT ") + kTargetColumns + " FROM targets t WHERE 1=1";
    std::vector<std::string> binds;

    // Whitespace-split terms, capped so a pathological query can't build an
    // enormous statement. Every term must match somewhere (AND), each term
    // may match any searchable field (OR).
    std::istringstream words(query.text);
    std::string term;
    for (int n = 0; n < 8 && words >> term; ++n) {
        sql += " AND (t.name LIKE ? ESCAPE '\\' OR t.description LIKE ? ESCAPE '\\'"
               " OR t.group_name LIKE ? ESCAPE '\\'"
               " OR EXISTS (SELECT 1 FROM target_tags g WHERE g.target_id = t.id"
               "            AND g.tag LIKE ? ESCAPE '\\'))";
        for (int i = 0; i < 4; ++i) binds.push_back(likeContains(term));
    }
    if (!query.tag.empty()) {
        sql += " AND EXISTS (SELECT 1 FROM target_tags g WHERE g.target_id = t.id AND g.tag = ?)";
        binds.push_back(query.tag);
    }
    if (!query.group.empty()) {
        sql += " AND t.group_name = ? COLLATE NOCASE";
        binds.push_back(query.group);
    }
    if (query.enabledOnly) sql += " AND t.enabled = 1";
    sql += " ORDER BY (t.group_name = ''), t.group_name COLLATE NOCASE, t.name COLLATE NOCASE";

    std::vector<Target> out;
    Stmt s(db_, sql);
    for (size_t i = 0; i < binds.size(); ++i) s.bind(static_cast<int>(i) + 1, binds[i]);
    for (;;) {
        auto step = s.step();
        if (step == Stmt::StepResult::Error) { reportFailure("listTargets", s); return {}; }
        if (step == Stmt::StepResult::Done) break;
        out.push_back(rowToTarget(s));
    }
    loadTags(out);
    return out;
}

std::optional<Database::Target> Database::getTarget(int64_t id) {
    Target t;
    {
        Stmt s(db_, std::string("SELECT ") + kTargetColumns + " FROM targets t WHERE t.id = ?");
        s.bind(1, static_cast<sqlite3_int64>(id));
        auto step = s.step();
        if (step == Stmt::StepResult::Error) { reportFailure("getTarget", s); return std::nullopt; }
        if (step != Stmt::StepResult::Row) return std::nullopt;
        t = rowToTarget(s);
    }
    Stmt s(db_, "SELECT tag FROM target_tags WHERE target_id = ? ORDER BY tag");
    s.bind(1, static_cast<sqlite3_int64>(id));
    while (s.step() == Stmt::StepResult::Row) t.tags.push_back(s.column_text(0));
    return t;
}

std::optional<int64_t> Database::defaultTargetId() {
    Stmt s(db_, "SELECT id FROM targets WHERE is_default = 1");
    auto step = s.step();
    if (step == Stmt::StepResult::Error) { reportFailure("defaultTargetId", s); return std::nullopt; }
    if (step != Stmt::StepResult::Row) return std::nullopt;
    return s.column_int64(0);
}

std::string Database::canonicalGroup(const std::string& group, int64_t exceptId) {
    if (group.empty()) return group;
    Stmt s(db_, "SELECT group_name FROM targets "
                "WHERE group_name = ? COLLATE NOCASE AND id != ? LIMIT 1");
    s.bind(1, group).bind(2, static_cast<sqlite3_int64>(exceptId));
    if (s.step() == Stmt::StepResult::Row) return s.column_text(0);
    return group;
}

bool Database::writeTags(int64_t id, const std::vector<std::string>& tags, DbOutcome* outcome) {
    {
        Stmt del(db_, "DELETE FROM target_tags WHERE target_id = ?");
        del.bind(1, static_cast<sqlite3_int64>(id));
        if (del.step() == Stmt::StepResult::Error) { reportFailure("writeTags", del); copyOutcome(del, outcome); return false; }
    }
    for (const auto& tag : tags) {
        Stmt ins(db_, "INSERT OR IGNORE INTO target_tags (target_id, tag) VALUES (?, ?)");
        ins.bind(1, static_cast<sqlite3_int64>(id)).bind(2, tag);
        if (ins.step() == Stmt::StepResult::Error) { reportFailure("writeTags", ins); copyOutcome(ins, outcome); return false; }
    }
    return true;
}

int64_t Database::createTarget(const Target& target, DbOutcome* outcome) {
    if (!beginTx(outcome)) return 0;
    int64_t id = 0;
    bool ok = false;
    {
        int64_t existing = 0;
        {
            Stmt c(db_, "SELECT COUNT(*) FROM targets");
            if (c.step() == Stmt::StepResult::Row) existing = c.column_int64(0);
        }
        Stmt s(db_, R"(
            INSERT INTO targets
                (name, description, group_name, enabled, is_default, v4l2_device,
                 capture_width, capture_height, capture_fps, qmp_socket, serial_device,
                 serial_baud, webrtc_bitrate_kbps, audio_device)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )");
        const auto& cfg = target.settings;
        s.bind(1, target.name)
         .bind(2, target.description)
         .bind(3, canonicalGroup(target.group, 0))
         .bind(4, target.enabled ? 1 : 0)
         .bind(5, existing == 0 ? 1 : 0)
         .bind(6, cfg.v4l2Device)
         .bind(7, static_cast<int>(cfg.captureWidth))
         .bind(8, static_cast<int>(cfg.captureHeight))
         .bind(9, static_cast<int>(cfg.captureFps))
         .bind(10, cfg.qmpSocket)
         .bind(11, cfg.serialDevice)
         .bind(12, cfg.serialBaud)
         .bind(13, static_cast<int>(cfg.webrtcBitrateKbps))
         .bind(14, cfg.audioDevice);
        if (s.step() == Stmt::StepResult::Error) {
            copyOutcome(s, outcome);
            if (!s.outcome().isConstraintViolation()) reportFailure("createTarget", s);
        } else {
            id = sqlite3_last_insert_rowid(db_);
            ok = writeTags(id, target.tags, outcome);
        }
    }
    endTx(ok);
    return ok ? id : 0;
}

bool Database::updateTarget(const Target& target, DbOutcome* outcome) {
    if (!beginTx(outcome)) return false;
    bool ok = false;
    {
        if (target.isDefault) {
            // Clear first: idx_targets_default allows only one flagged row.
            Stmt clear(db_, "UPDATE targets SET is_default = 0 WHERE is_default = 1 AND id != ?");
            clear.bind(1, static_cast<sqlite3_int64>(target.id));
            if (clear.step() == Stmt::StepResult::Error) {
                reportFailure("updateTarget", clear);
                copyOutcome(clear, outcome);
                endTx(false);
                return false;
            }
        }
        Stmt s(db_, R"(
            UPDATE targets SET
                name = ?, description = ?, group_name = ?, enabled = ?,
                is_default = CASE WHEN ? THEN 1 ELSE is_default END,
                v4l2_device = ?, capture_width = ?, capture_height = ?, capture_fps = ?,
                qmp_socket = ?, serial_device = ?, serial_baud = ?, webrtc_bitrate_kbps = ?,
                audio_device = ?, updated_at = datetime('now')
            WHERE id = ?
        )");
        const auto& cfg = target.settings;
        s.bind(1, target.name)
         .bind(2, target.description)
         .bind(3, canonicalGroup(target.group, target.id))
         .bind(4, target.enabled ? 1 : 0)
         .bind(5, target.isDefault ? 1 : 0)
         .bind(6, cfg.v4l2Device)
         .bind(7, static_cast<int>(cfg.captureWidth))
         .bind(8, static_cast<int>(cfg.captureHeight))
         .bind(9, static_cast<int>(cfg.captureFps))
         .bind(10, cfg.qmpSocket)
         .bind(11, cfg.serialDevice)
         .bind(12, cfg.serialBaud)
         .bind(13, static_cast<int>(cfg.webrtcBitrateKbps))
         .bind(14, cfg.audioDevice)
         .bind(15, static_cast<sqlite3_int64>(target.id));
        if (s.step() == Stmt::StepResult::Error) {
            copyOutcome(s, outcome);
            if (!s.outcome().isConstraintViolation()) reportFailure("updateTarget", s);
        } else {
            ok = writeTags(target.id, target.tags, outcome);
        }
    }
    endTx(ok);
    return ok;
}

bool Database::deleteTarget(int64_t id, DbOutcome* outcome) {
    Stmt s(db_, "DELETE FROM targets WHERE id = ?"); // target_tags cascades
    s.bind(1, static_cast<sqlite3_int64>(id));
    if (s.step() == Stmt::StepResult::Error) {
        reportFailure("deleteTarget", s);
        copyOutcome(s, outcome);
        return false;
    }
    return true;
}

std::string Database::iceServersJson() {
    Stmt s(db_, "SELECT ice_servers FROM server_settings WHERE id = 1");
    auto step = s.step();
    if (step == Stmt::StepResult::Error) { reportFailure("iceServersJson", s); return "[]"; }
    if (step != Stmt::StepResult::Row) return "[]";
    return s.column_text(0);
}

bool Database::setIceServersJson(const std::string& json, DbOutcome* outcome) {
    Stmt s(db_, "UPDATE server_settings SET ice_servers = ?, updated_at = datetime('now') WHERE id = 1");
    s.bind(1, json);
    if (s.step() == Stmt::StepResult::Error) {
        reportFailure("setIceServersJson", s);
        copyOutcome(s, outcome);
        return false;
    }
    return true;
}

Database::TlsSettings Database::tlsSettings() {
    TlsSettings t;
    Stmt s(db_, "SELECT tls_enabled, https_port, http_redirect, hsts, tls_auto_renew "
                "FROM server_settings WHERE id = 1");
    auto step = s.step();
    if (step == Stmt::StepResult::Error) { reportFailure("tlsSettings", s); return t; }
    if (step != Stmt::StepResult::Row) return t;
    t.enabled      = s.column_int(0) != 0;
    t.httpsPort    = s.column_int(1);
    t.httpRedirect = s.column_int(2) != 0;
    t.hsts         = s.column_int(3) != 0;
    t.autoRenew    = s.column_int(4) != 0;
    return t;
}

bool Database::setTlsSettings(const TlsSettings& t, DbOutcome* outcome) {
    Stmt s(db_, "UPDATE server_settings SET tls_enabled = ?, https_port = ?, http_redirect = ?, hsts = ?, "
                "tls_auto_renew = ?, updated_at = datetime('now') WHERE id = 1");
    s.bind(1, t.enabled ? 1 : 0);
    s.bind(2, t.httpsPort);
    s.bind(3, t.httpRedirect ? 1 : 0);
    s.bind(4, t.hsts ? 1 : 0);
    s.bind(5, t.autoRenew ? 1 : 0);
    if (s.step() == Stmt::StepResult::Error) {
        reportFailure("setTlsSettings", s);
        copyOutcome(s, outcome);
        return false;
    }
    return true;
}

std::vector<Database::FacetCount> Database::listGroups() {
    std::vector<FacetCount> out;
    Stmt s(db_, "SELECT MIN(group_name), COUNT(*) FROM targets WHERE group_name != '' "
                "GROUP BY group_name COLLATE NOCASE ORDER BY group_name COLLATE NOCASE");
    while (s.step() == Stmt::StepResult::Row)
        out.push_back({s.column_text(0), s.column_int(1)});
    return out;
}

std::vector<Database::FacetCount> Database::listTags() {
    std::vector<FacetCount> out;
    Stmt s(db_, "SELECT tag, COUNT(*) AS n FROM target_tags GROUP BY tag ORDER BY n DESC, tag");
    while (s.step() == Stmt::StepResult::Row)
        out.push_back({s.column_text(0), s.column_int(1)});
    return out;
}

bool Database::execute(const std::string& sql, DbOutcome* outcome) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (outcome) {
            outcome->ok = false;
            outcome->sqliteCode = rc;
            outcome->message = errMsg ? errMsg : "Failed to execute SQL";
        }
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

Stmt::Stmt(sqlite3* db, const std::string& sql) : db_(db) {
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        outcome_.message = "Failed to prepare statement: " + std::string(sqlite3_errmsg(db_));
        outcome_.sqliteCode = rc;
        outcome_.ok = false;
        ok_ = false;
        stmt_ = nullptr;
    }
}

Stmt::~Stmt() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
    }
}

void Stmt::fail(int rc) {
    outcome_.ok = false;
    outcome_.sqliteCode = rc;
    outcome_.message = sqlite3_errmsg(db_);
    ok_ = false;
}

Stmt& Stmt::bind(int idx, int value) {
    if (!ok_) return *this;
    int rc = sqlite3_bind_int(stmt_, idx, value);
    if (rc != SQLITE_OK) fail(rc);
    return *this;
}

Stmt& Stmt::bind(int idx, sqlite3_int64 value) {
    if (!ok_) return *this;
    int rc = sqlite3_bind_int64(stmt_, idx, value);
    if (rc != SQLITE_OK) fail(rc);
    return *this;
}

Stmt& Stmt::bind(int idx, const std::string& value) {
    if (!ok_) return *this;
    int rc = sqlite3_bind_text(stmt_, idx, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) fail(rc);
    return *this;
}

Stmt& Stmt::bindNull(int idx) {
    if (!ok_) return *this;
    int rc = sqlite3_bind_null(stmt_, idx);
    if (rc != SQLITE_OK) fail(rc);
    return *this;
}

Stmt::StepResult Stmt::step() {
    if (!ok_) return StepResult::Error;
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)  return StepResult::Row;
    if (rc == SQLITE_DONE) return StepResult::Done;
    fail(rc);
    return StepResult::Error;
}

int Stmt::column_int(int idx) const {
    if (!ok_) return 0;
    return sqlite3_column_int(stmt_, idx);
}

sqlite3_int64 Stmt::column_int64(int idx) const {
    if (!ok_) return 0;
    return sqlite3_column_int64(stmt_, idx);
}

std::string Stmt::column_text(int idx) const {
    if (!ok_) return "";
    const unsigned char* text = sqlite3_column_text(stmt_, idx);
    return text ? reinterpret_cast<const char*>(text) : "";
}

bool Stmt::reset() {
    if (!ok_) return false;
    int rc = sqlite3_reset(stmt_);
    if (rc != SQLITE_OK) { fail(rc); return false; }
    return true;
}


}
