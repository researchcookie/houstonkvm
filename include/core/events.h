#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace houston_kvm {

// Who did something: the account (0 / "" when nobody is signed in, as for a
// failed sign-in, where username is the name that was tried), where the
// request came from, and whether it came with an API token rather than a
// browser session.
struct Actor {
    int64_t     userId = 0;
    std::string username;
    std::string ip;
    bool        viaToken = false;
};

namespace event {
// Outcomes.
inline constexpr std::string_view kOk     = "ok";
inline constexpr std::string_view kDenied = "denied";  // refused: bad credentials, rate limited
inline constexpr std::string_view kBusy   = "busy";    // refused: server overloaded (503)
inline constexpr std::string_view kFailed = "failed";  // tried, and it didn't work

// Accounts and sign-in.
inline constexpr std::string_view kSetup           = "auth.setup";
inline constexpr std::string_view kLogin           = "auth.login";
inline constexpr std::string_view kLogout          = "auth.logout";
inline constexpr std::string_view kPasswordChanged = "auth.password_change";
inline constexpr std::string_view kTokenCreated    = "token.create";
inline constexpr std::string_view kTokenRevoked    = "token.revoke";
inline constexpr std::string_view kUserCreated     = "user.create";
inline constexpr std::string_view kUserRoleChanged = "user.role_change";
inline constexpr std::string_view kUserDeleted     = "user.delete";

// Targets and their hardware.
inline constexpr std::string_view kTargetCreated   = "target.create";
inline constexpr std::string_view kTargetUpdated   = "target.update";
inline constexpr std::string_view kTargetDeleted   = "target.delete";
inline constexpr std::string_view kVideoSettings   = "target.video_settings";
inline constexpr std::string_view kInputTest       = "target.input_test";
inline constexpr std::string_view kBaudChange      = "target.baud_change";

// Server-wide settings.
inline constexpr std::string_view kIceServers      = "server.ice_servers";

// HTTPS (tls/tls_manager.h). Never any key material.
inline constexpr std::string_view kTlsCaCreated     = "tls.ca_created";
inline constexpr std::string_view kTlsCsrCreated    = "tls.csr_created";
inline constexpr std::string_view kTlsCertInstalled = "tls.certificate_installed";
inline constexpr std::string_view kTlsEnabled       = "tls.enabled";    // confirmed over HTTPS
inline constexpr std::string_view kTlsReverted      = "tls.reverted";   // not confirmed in time
inline constexpr std::string_view kTlsDisabled      = "tls.disabled";
inline constexpr std::string_view kTlsReloaded      = "tls.reloaded";   // SIGHUP: certificate re-read
inline constexpr std::string_view kTlsSettings      = "tls.settings";

// Driving a target.
inline constexpr std::string_view kControlAcquired = "control.acquire";
inline constexpr std::string_view kControlReleased = "control.release";
inline constexpr std::string_view kControlForced   = "control.force_release";
} // namespace event

// Something that happened on the server that an administrator may later
// need to account for. Events are what the audit log records, and what a
// metrics endpoint counts: both subscribe to the same EventBus, so a route
// reports each action once. `type` and `outcome` come from the fixed lists
// above, which keeps them usable as metric labels; free-form facts go in
// `detail`, which only the audit log keeps.
//
// What a driver types is never recorded anywhere, events included: control
// sessions only count key presses and mouse input (see ControlHooks).
struct Event {
    Event(std::string_view type, std::string_view outcome = event::kOk, Actor actor = {})
        : type(type), outcome(outcome), actor(std::move(actor)) {}

    std::string_view type;
    std::string_view outcome;
    Actor            actor;
    int64_t          targetId = 0;
    std::string      targetName;
    nlohmann::json   detail = nlohmann::json::object();
};

// Synchronous fan-out to subscribers. Emit and subscribe only on the uWS
// event-loop thread, like the routes that emit; subscribers must be quick.
class EventBus {
public:
    using Subscriber = std::function<void(const Event&)>;

    void subscribe(Subscriber s) { subscribers_.push_back(std::move(s)); }

    void emit(const Event& e) const {
        for (const auto& s : subscribers_) s(e);
    }

private:
    std::vector<Subscriber> subscribers_;
};

} // namespace houston_kvm
