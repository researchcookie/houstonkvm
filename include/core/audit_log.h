#pragma once

#include "core/database.h"
#include "core/events.h"

#include <App.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace houston_kvm {

// The server's audit log, kept in its own database tables:
//   - audit_events: every Event on the EventBus (sign-ins, account, token
//     and target changes, taking and releasing control, ...);
//   - audit_control_sessions: each stretch of time someone drove a target,
//     from taking control to losing it, why it ended, and how much they
//     typed and moved the mouse meanwhile.
//
// Only how MUCH: never which keys or where the mouse went. A record of the
// keys would hold every password typed into a target, and anything the API
// can read can leak. The control hooks don't even pass the key along.
//
// Kept for `retentionDays` (pruned hourly). Activity counts are written
// once a second, not per key, so typing never waits on disk.
//
// Everything here runs on the uWS event-loop thread.
class AuditLog {
public:
    AuditLog(Database& db, uWS::Loop* loop, int retentionDays);
    ~AuditLog();

    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;

    // Marks control sessions a crash left open, prunes, and starts the
    // flush/prune timer. Call once, before the event loop runs.
    void start();
    // Writes what is buffered, ends every open control session as
    // "server_stopped", and stops the timer. Call at shutdown.
    void stop();

    void record(const Event& e);

    // One driver's stretch of control over one target. begin returns the
    // session's id (0 if it couldn't be recorded; the other calls then do
    // nothing). keyPressed and mouse only count.
    int64_t beginControl(int64_t targetId, const std::string& targetName, const Actor& driver);
    void keyPressed(int64_t sessionId);
    void mouse(int64_t sessionId);
    void endControl(int64_t sessionId, std::string_view reason);

    // Queries for the API. Newest first; `beforeId` pages back through older
    // rows (the previous page's last id).
    struct EventFilter {
        std::string type;       // exact type, or a prefix ending in '.' ("auth.")
        int64_t     userId = 0;
        int64_t     targetId = 0;
        std::string since, until;   // ISO 8601, compared as text
        int64_t     beforeId = 0;
        int         limit = 100;
    };
    std::optional<nlohmann::json> events(const EventFilter& f, DbOutcome& outcome);

    struct SessionFilter {
        int64_t userId = 0;
        int64_t targetId = 0;
        int64_t beforeId = 0;
        int     limit = 100;
    };
    std::optional<nlohmann::json> controlSessions(const SessionFilter& f, DbOutcome& outcome);

    // Writes the running activity counts now (the API reads after this).
    void flush();

private:
    struct OpenSession {
        int64_t keys = 0;    // presses
        int64_t mouse = 0;   // moves, clicks, scrolls
    };

    static void onTimer(struct us_timer_t* timer);
    void prune();

    Database&   db_;
    uWS::Loop*  loop_;
    const int   retentionDays_;
    struct us_timer_t* timer_ = nullptr;
    int         ticks_ = 0;

    std::unordered_map<int64_t, OpenSession> open_;
    bool                                     dirty_ = false;   // counts changed since flush()
};

} // namespace houston_kvm
