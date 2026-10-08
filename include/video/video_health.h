#pragma once

#include <chrono>
#include <string>

namespace houston_kvm {

// What a target's video is doing, for the directory's status tag and
// GET /api/targets/:id/health. A capture device that stops or can't be
// opened is retried by StreamManager with backoff, so every state but
// Good and Off is expected to change by itself.
enum class VideoState {
    Off,           // no capture device set for this target
    Starting,      // opened, no frame yet
    Good,          // frames arriving
    NoSignal,      // device open, but no frame for a few seconds
    Reconnecting,  // it was working and stopped; reopening it
    Absent,        // can't be opened since the target (re)started; retrying
};

inline const char* videoStateName(VideoState s) {
    switch (s) {
        case VideoState::Off:          return "off";
        case VideoState::Starting:     return "starting";
        case VideoState::Good:         return "good";
        case VideoState::NoSignal:     return "no_signal";
        case VideoState::Reconnecting: return "reconnecting";
        case VideoState::Absent:       return "absent";
    }
    return "off";
}

struct VideoHealth {
    VideoState  state = VideoState::Starting;
    std::string device;
    // Why the last open failed or the capture stopped, kept after it
    // recovers. The device path is replaced: health is for every viewer.
    std::string lastError;
    int         reconnects = 0; // times the video came back by itself since the target started
    std::chrono::steady_clock::time_point retryAt{};  // Reconnecting/Absent: the next attempt
};

} // namespace houston_kvm
