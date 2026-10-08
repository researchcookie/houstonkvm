#pragma once

#include <cstdint>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace houston_kvm {

struct DbOutcome {
    bool ok = true;
    int sqliteCode = 0;
    std::string message;

    bool isConstraintViolation() const noexcept { return !ok && (sqliteCode & 0xFF) == SQLITE_CONSTRAINT; }
};

// Thin RAII wrapper 
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return isOpen_; }
    const std::string& openError() const noexcept { return openError_; }

    [[nodiscard]] bool execute(const std::string& sql, DbOutcome* outcome = nullptr);

    // Creates the schema if it doesn't exist yet. Exits the process on
    // failure — schema setup has always been fail-fast at boot.
    void init();

    // Applies any schema migrations not yet applied (tracked via
    // PRAGMA user_version). Exits the process on failure.
    void migrate();

    // Per-target capture + input wiring (capture device/mode, input backend
    // selection, WebRTC bitrate). Distinct from user_settings, which holds
    // per-user UI preferences. Which backend a target uses follows the same
    // precedence StreamManager always applied: QEMU QMP socket, then CH9329
    // serial device, then the local USB-HID-gadget fallback.
    struct TargetSettings {
        std::string v4l2Device      = "/dev/video0";
        uint32_t    captureWidth    = 1280;
        uint32_t    captureHeight   = 720;
        uint32_t    captureFps      = 30;
        std::string qmpSocket;
        std::string serialDevice;
        int         serialBaud      = 9600;
        uint32_t    webrtcBitrateKbps = 4000;
        std::string audioDevice     = "auto";   // see video/audio_capture.h

        bool operator==(const TargetSettings&) const = default;
    };

    // One controllable machine: what an admin sees/searches (name,
    // description, group, tags) plus the hardware wiring in `settings`.
    struct Target {
        int64_t                  id = 0;
        std::string              name;        // unique, case-insensitive
        std::string              description;
        std::string              group;       // free text, e.g. "Rack 3"
        std::vector<std::string> tags;        // lowercase, sorted
        bool                     enabled = true;   // disabled = no capture/encode/input running
        bool                     isDefault = false; // what the unscoped /api/* routes address
        TargetSettings           settings;
    };

    struct TargetQuery {
        // Whitespace-separated terms; every term must appear (substring,
        // case-insensitive) in the name, description, group, or some tag.
        std::string text;
        std::string tag;     // exact tag (lowercase)
        std::string group;   // exact group, case-insensitive
        bool        enabledOnly = false;
    };

    struct FacetCount {
        std::string value;
        int         count = 0;
    };

    // Ordered by group (ungrouped last), then name.
    std::vector<Target> listTargets(const TargetQuery& query);
    std::vector<Target> listTargets() { return listTargets(TargetQuery{}); }
    std::optional<Target> getTarget(int64_t id);
    // The target the unscoped /api/* routes address. Explicit rather than
    // "lowest id" so an admin deleting or disabling a target can never
    // silently re-point an unscoped client (e.g. an automated driver) at a
    // different machine — with no default, those routes fail instead.
    std::optional<int64_t> defaultTargetId();

    // createTarget returns the new id (0 on failure, see *outcome). The very
    // first target ever created becomes the default. Group spelling is
    // snapped to an existing group's case-insensitive match, so "rack 3" and
    // "Rack 3" can't drift into two groups.
    int64_t createTarget(const Target& target, DbOutcome* outcome = nullptr);
    // Replaces every editable column and the tag set. target.isDefault ==
    // true moves the default flag here; false leaves it untouched (there is
    // no "unset" — designate a different default instead).
    bool updateTarget(const Target& target, DbOutcome* outcome = nullptr);
    bool deleteTarget(int64_t id, DbOutcome* outcome = nullptr);

    std::vector<FacetCount> listGroups();
    std::vector<FacetCount> listTags(); // most-used first

    // Server-wide STUN/TURN servers as stored: a JSON list in the form
    // webrtc/ice_servers.h reads ("[]" = none). Validated by the caller.
    std::string iceServersJson();
    bool setIceServersJson(const std::string& json, DbOutcome* outcome = nullptr);

    // HTTPS (tls/tls_manager.h): whether an Owner turned it on, and how it's
    // served. The certificate is kept in files, not here.
    struct TlsSettings {
        bool enabled      = false;
        int  httpsPort    = 8443;
        bool httpRedirect = true;    // once HTTPS is on, the HTTP port only redirects
        bool hsts         = false;
        bool autoRenew    = true;    // only ever applies to HoustonKVM's own CA's certificates
    };
    TlsSettings tlsSettings();
    bool setTlsSettings(const TlsSettings& settings, DbOutcome* outcome = nullptr);

    sqlite3* handle() { return db_; }
private:
    bool beginTx(DbOutcome* outcome);
    bool endTx(bool commit);
    void loadTags(std::vector<Target>& targets);
    std::string canonicalGroup(const std::string& group, int64_t exceptId);
    bool writeTags(int64_t id, const std::vector<std::string>& tags, DbOutcome* outcome);

    // Runs sql, exiting the process with a fatal error on failure.
    void mustExecute(const std::string& sql);

    sqlite3* db_ = nullptr;
    bool isOpen_ = false;
    std::string openError_;
};

class Stmt {
public:
    enum class StepResult { Row, Done, Error };

    Stmt(sqlite3* db, const std::string& sql);
    ~Stmt();

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int index, const std::string& value);
    Stmt& bind(int index, int value);
    Stmt& bind(int index, sqlite3_int64 value);
    Stmt& bindNull(int index);

    [[nodiscard]] StepResult step();

    int column_int(int idx) const;
    sqlite3_int64 column_int64(int idx) const;
    std::string column_text(int idx) const;
    [[nodiscard]] bool reset();

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    const DbOutcome& outcome() const noexcept { return outcome_; }
private:
    void fail(int rc);

    sqlite3_stmt* stmt_ = nullptr;
    sqlite3* db_ = nullptr;
    bool ok_ = true;
    DbOutcome outcome_;
};


} // namespace houston_kvm