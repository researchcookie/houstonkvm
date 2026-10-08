#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace houston_kvm {

// How well a target's keyboard/mouse link is working, for the directory's
// health tag, GET /api/targets/:id/health and the on-demand input test.
enum class HealthState { Unknown, Good, Degraded, Down };

inline const char* healthStateName(HealthState s) {
    switch (s) {
        case HealthState::Good:     return "good";
        case HealthState::Degraded: return "degraded";
        case HealthState::Down:     return "down";
        case HealthState::Unknown:  break;
    }
    return "unknown";
}

struct InputHealth {
    std::string kind;       // "ch9329", "usb-gadget", "qemu"
    HealthState state = HealthState::Unknown;
    // One sentence for a person: what's wrong and which cable to look at.
    std::string summary;

    // Measured by backends that can talk back (CH9329); unset otherwise.
    std::optional<bool> deviceOpen;
    std::optional<bool> targetEnumerated;   // the target machine recognised the adapter
    std::optional<int>  firmwareVersion;    // raw byte, e.g. 0x30 = 3.0
    std::optional<int>  configuredBaud;     // this target's setting
    std::optional<int>  chipBaud;           // stored in the chip's own configuration
    std::optional<int>  answersAtBaud;      // found by scanning after the chip went quiet

    // Rolling window: the last hour.
    int    checksOk = 0;
    int    checksFailed = 0;
    double replyMsAvg = 0;
    double replyMsMax = 0;
    int    reconnects = 0;
    int    droppedCommands = 0;
};

// Result of an on-demand test: a burst of probes plus a configuration read.
struct InputSelfTest {
    enum class Phase { Idle, Running, Done };
    Phase phase = Phase::Idle;

    int    probes = 0;
    int    ok = 0;
    double replyMsAvg = 0;
    double replyMsMax = 0;
    std::optional<bool> targetEnumerated;
    std::optional<int>  firmwareVersion;
    std::optional<int>  chipBaud;
    std::optional<int>  workMode;
    std::optional<int>  serialMode;

    HealthState verdictState = HealthState::Unknown;
    std::string verdict;
    int64_t     finishedAtUnix = 0;
};

// A change of the adapter's line speed (see InputBackend::startBaudChange()).
struct InputBaudChange {
    enum class Phase { Idle, Running, Done };
    Phase phase = Phase::Idle;

    int  fromBaud = 0;
    int  toBaud = 0;
    bool ok = false;
    std::string message;   // one sentence for a person
    int64_t     finishedAtUnix = 0;
};

} // namespace houston_kvm
