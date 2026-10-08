#include "core/audit_log.h"

#include <sqlite3.h>

#include <iostream>

namespace houston_kvm {

using json = nlohmann::json;

namespace {

constexpr int kFlushIntervalMs  = 1000;
constexpr int kPruneEveryTicks  = 3600;   // hourly, at one tick a second

std::string via(const Actor& a) { return a.viaToken ? "token" : "session"; }

void bindOptionalId(Stmt& s, int index, int64_t id) {
    if (id > 0) s.bind(index, static_cast<sqlite3_int64>(id));
    else s.bindNull(index);
}

json userJson(const Stmt& s, int idCol, int nameCol) {
    auto id = s.column_int64(idCol);
    return json{{"id", id > 0 ? json(id) : json(nullptr)}, {"username", s.column_text(nameCol)}};
}

} // namespace

AuditLog::AuditLog(Database& db, uWS::Loop* loop, int retentionDays)
    : db_(db), loop_(loop), retentionDays_(retentionDays) {}

AuditLog::~AuditLog() { stop(); }

void AuditLog::start() {
    // Nothing is known about when these really ended: leave ended_at empty.
    if (!db_.execute("UPDATE audit_control_sessions SET end_reason = 'interrupted' "
                     "WHERE end_reason IS NULL"))
        std::cerr << "Audit: could not close interrupted control sessions\n";
    prune();
    // fallthrough 0 and an explicit close in stop(): see
    // TargetManager::startDriverRevalidation for why.
    timer_ = us_create_timer(reinterpret_cast<us_loop_t*>(loop_), 0, sizeof(AuditLog*));
    *static_cast<AuditLog**>(us_timer_ext(timer_)) = this;
    us_timer_set(timer_, &AuditLog::onTimer, kFlushIntervalMs, kFlushIntervalMs);
}

void AuditLog::stop() {
    if (!timer_) return;
    std::vector<int64_t> ids;
    for (const auto& [id, s] : open_) ids.push_back(id);
    for (auto id : ids) endControl(id, "server_stopped");
    flush();
    us_timer_close(timer_);
    timer_ = nullptr;
}

void AuditLog::onTimer(struct us_timer_t* timer) {
    auto* self = *static_cast<AuditLog**>(us_timer_ext(timer));
    self->flush();
    if (++self->ticks_ >= kPruneEveryTicks) {
        self->ticks_ = 0;
        self->prune();
    }
}

void AuditLog::prune() {
    const std::string cutoff = "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-" +
                               std::to_string(retentionDays_) + " days')";
    DbOutcome outcome;
    if (!db_.execute("DELETE FROM audit_events WHERE at < " + cutoff, &outcome))
        std::cerr << "Audit: pruning events failed: " << outcome.message << "\n";
    // A session still open is kept whatever its age.
    if (!db_.execute("DELETE FROM audit_control_sessions WHERE started_at < " + cutoff +
                     " AND end_reason IS NOT NULL", &outcome))
        std::cerr << "Audit: pruning control sessions failed: " << outcome.message << "\n";
}

void AuditLog::record(const Event& e) {
    Stmt ins(db_.handle(),
        "INSERT INTO audit_events (type, outcome, user_id, username, ip, via, target_id, target_name, detail) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, std::string(e.type)).bind(2, std::string(e.outcome));
    bindOptionalId(ins, 3, e.actor.userId);
    ins.bind(4, e.actor.username).bind(5, e.actor.ip)
       .bind(6, e.actor.userId == 0 && e.actor.username.empty() ? "" : via(e.actor));
    bindOptionalId(ins, 7, e.targetId);
    ins.bind(8, e.targetName).bind(9, e.detail.dump());
    if (ins.step() == Stmt::StepResult::Error)
        std::cerr << "Audit: could not record " << e.type << ": " << ins.outcome().message << "\n";
}

int64_t AuditLog::beginControl(int64_t targetId, const std::string& targetName, const Actor& driver) {
    Stmt ins(db_.handle(),
        "INSERT INTO audit_control_sessions (target_id, target_name, user_id, username, ip, via) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    ins.bind(1, static_cast<sqlite3_int64>(targetId)).bind(2, targetName);
    bindOptionalId(ins, 3, driver.userId);
    ins.bind(4, driver.username).bind(5, driver.ip).bind(6, via(driver));
    if (ins.step() == Stmt::StepResult::Error) {
        std::cerr << "Audit: could not record a control session: " << ins.outcome().message << "\n";
        return 0;
    }
    int64_t id = sqlite3_last_insert_rowid(db_.handle());
    open_[id] = OpenSession{};
    return id;
}

void AuditLog::keyPressed(int64_t sessionId) {
    auto it = open_.find(sessionId);
    if (it == open_.end()) return;
    ++it->second.keys;
    dirty_ = true;
}

void AuditLog::mouse(int64_t sessionId) {
    auto it = open_.find(sessionId);
    if (it == open_.end()) return;
    ++it->second.mouse;
    dirty_ = true;
}

void AuditLog::endControl(int64_t sessionId, std::string_view reason) {
    auto it = open_.find(sessionId);
    if (it == open_.end()) return;
    Stmt upd(db_.handle(),
        "UPDATE audit_control_sessions SET ended_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "end_reason = ?, key_count = ?, mouse_count = ? WHERE id = ?");
    upd.bind(1, std::string(reason))
       .bind(2, static_cast<sqlite3_int64>(it->second.keys))
       .bind(3, static_cast<sqlite3_int64>(it->second.mouse))
       .bind(4, static_cast<sqlite3_int64>(sessionId));
    if (upd.step() == Stmt::StepResult::Error)
        std::cerr << "Audit: could not close control session " << sessionId << ": "
                  << upd.outcome().message << "\n";
    open_.erase(it);
}

// Running totals, so a session still in progress shows them too.
void AuditLog::flush() {
    if (!dirty_) return;
    DbOutcome outcome;
    if (!db_.execute("BEGIN", &outcome)) {
        std::cerr << "Audit: flush failed: " << outcome.message << "\n";
        return;
    }
    bool ok = true;
    {
        Stmt upd(db_.handle(),
            "UPDATE audit_control_sessions SET key_count = ?, mouse_count = ? WHERE id = ?");
        for (const auto& [id, s] : open_) {
            upd.bind(1, static_cast<sqlite3_int64>(s.keys))
               .bind(2, static_cast<sqlite3_int64>(s.mouse))
               .bind(3, static_cast<sqlite3_int64>(id));
            if (upd.step() == Stmt::StepResult::Error || !upd.reset()) { ok = false; break; }
        }
    }
    if (ok && db_.execute("COMMIT", &outcome)) {
        dirty_ = false;
        return;
    }
    std::cerr << "Audit: writing activity counts failed; will retry\n";
    (void)db_.execute("ROLLBACK");
}

std::optional<json> AuditLog::events(const EventFilter& f, DbOutcome& outcome) {
    std::string sql =
        "SELECT id, at, type, outcome, COALESCE(user_id, 0), username, ip, via, "
        "COALESCE(target_id, 0), target_name, detail FROM audit_events WHERE 1=1";
    const bool typePrefix = !f.type.empty() && f.type.back() == '.';
    if (!f.type.empty()) sql += typePrefix ? " AND substr(type, 1, ?) = ?" : " AND type = ?";
    if (f.userId)       sql += " AND user_id = ?";
    if (f.targetId)     sql += " AND target_id = ?";
    if (!f.since.empty()) sql += " AND at >= ?";
    if (!f.until.empty()) sql += " AND at < ?";
    if (f.beforeId)     sql += " AND id < ?";
    sql += " ORDER BY id DESC LIMIT ?";

    Stmt s(db_.handle(), sql);
    int i = 1;
    if (!f.type.empty()) {
        if (typePrefix) s.bind(i++, static_cast<int>(f.type.size()));
        s.bind(i++, f.type);
    }
    if (f.userId)         s.bind(i++, static_cast<sqlite3_int64>(f.userId));
    if (f.targetId)       s.bind(i++, static_cast<sqlite3_int64>(f.targetId));
    if (!f.since.empty()) s.bind(i++, f.since);
    if (!f.until.empty()) s.bind(i++, f.until);
    if (f.beforeId)       s.bind(i++, static_cast<sqlite3_int64>(f.beforeId));
    s.bind(i++, f.limit);

    json rows = json::array();
    for (;;) {
        auto step = s.step();
        if (step == Stmt::StepResult::Error) { outcome = s.outcome(); return std::nullopt; }
        if (step == Stmt::StepResult::Done) break;
        auto targetId = s.column_int64(8);
        rows.push_back({
            {"id", s.column_int64(0)}, {"at", s.column_text(1)},
            {"type", s.column_text(2)}, {"outcome", s.column_text(3)},
            {"user", userJson(s, 4, 5)}, {"ip", s.column_text(6)}, {"via", s.column_text(7)},
            {"target", targetId > 0 ? json{{"id", targetId}, {"name", s.column_text(9)}} : json(nullptr)},
            {"detail", json::parse(s.column_text(10), nullptr, false)},
        });
    }
    return rows;
}

namespace {

constexpr const char* kSessionColumns =
    "id, target_id, target_name, COALESCE(user_id, 0), username, ip, via, started_at, "
    "COALESCE(ended_at, ''), COALESCE(end_reason, ''), key_count, mouse_count";

json sessionJson(const Stmt& s) {
    auto ended = s.column_text(8), reason = s.column_text(9);
    return json{
        {"id", s.column_int64(0)},
        {"target", {{"id", s.column_int64(1)}, {"name", s.column_text(2)}}},
        {"user", userJson(s, 3, 4)}, {"ip", s.column_text(5)}, {"via", s.column_text(6)},
        {"started_at", s.column_text(7)},
        {"ended_at", ended.empty() ? json(nullptr) : json(ended)},
        {"end_reason", reason.empty() ? json(nullptr) : json(reason)},
        {"key_count", s.column_int64(10)}, {"mouse_count", s.column_int64(11)},
    };
}

} // namespace

std::optional<json> AuditLog::controlSessions(const SessionFilter& f, DbOutcome& outcome) {
    flush();
    std::string sql = std::string("SELECT ") + kSessionColumns + " FROM audit_control_sessions WHERE 1=1";
    if (f.userId)   sql += " AND user_id = ?";
    if (f.targetId) sql += " AND target_id = ?";
    if (f.beforeId) sql += " AND id < ?";
    sql += " ORDER BY id DESC LIMIT ?";
    Stmt s(db_.handle(), sql);
    int i = 1;
    if (f.userId)   s.bind(i++, static_cast<sqlite3_int64>(f.userId));
    if (f.targetId) s.bind(i++, static_cast<sqlite3_int64>(f.targetId));
    if (f.beforeId) s.bind(i++, static_cast<sqlite3_int64>(f.beforeId));
    s.bind(i++, f.limit);

    json rows = json::array();
    for (;;) {
        auto step = s.step();
        if (step == Stmt::StepResult::Error) { outcome = s.outcome(); return std::nullopt; }
        if (step == Stmt::StepResult::Done) break;
        rows.push_back(sessionJson(s));
    }
    return rows;
}

} // namespace houston_kvm
