#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace houston_kvm {

// A target's sound: most USB HDMI capture dongles are also a USB sound card
// carrying the HDMI audio, which ALSA exposes as its own card. AudioCapture
// reads it and encodes it to Opus for the WebRTC publisher's audio track.
//
// A target's audio_device setting says where the sound comes from:
//   ""       no sound
//   "auto"   the sound card on the same USB device as the target's capture
//            device, found afresh every time it's opened (a re-plugged
//            dongle can come back as a different card number)
//   "plughw:CARD=Video,DEV=0" and the like: that ALSA device. Only hw,
//            plughw and sysdefault names are accepted (isValidAudioSetting),
//            so the setting can't name ALSA plugins that touch files.
// In load-test builds "synthetic:tone" (optionally "/<name>") plays a
// steady tone instead, for testing without a sound card.

enum class AudioState {
    Off,          // no sound configured
    Unsupported,  // configured, but this build has no sound support
    Idle,         // nobody is watching over WebRTC, so nothing is captured
    Good,         // capturing and sending
    Absent,       // can't open the sound device; retrying
};
const char* audioStateName(AudioState state);

struct AudioHealth {
    AudioState  state = AudioState::Off;
    std::string device;    // the ALSA device in use or last tried
    std::string lastError; // why it isn't capturing, when it isn't
};

// Whether this build can capture sound at all (built with ALSA and Opus).
bool audioSupported();

bool isValidAudioSetting(std::string_view setting);

// A sound card that can record, for the target editor's picker.
struct AudioDevice {
    std::string device; // "plughw:CARD=<id>,DEV=<n>"
    std::string name;   // the card's own name, e.g. "USB2 Video"
};
std::vector<AudioDevice> enumerateAudioDevices();

// The ALSA device "auto" means for a capture device: the first recording
// device of the sound card on the same USB device. Empty, with `error` set,
// when there isn't one.
std::string audioDeviceFor(const std::string& v4l2Device, std::string* error = nullptr);

// Captures one target's sound on its own thread while `wanted()` says
// someone is listening (it's polled; nothing is opened otherwise), and hands
// each 20 ms Opus frame to `onFrame` along with when it was captured. The
// device is reopened with backoff if it can't be opened or goes away.
class AudioCapture {
public:
    using Clock         = std::chrono::steady_clock;
    using FrameCallback = std::function<void(const uint8_t* opus, size_t size, Clock::time_point captured)>;

    // `v4l2Device` is only used to resolve "auto".
    AudioCapture(std::string setting, std::string v4l2Device,
                 std::function<bool()> wanted, FrameCallback onFrame);
    ~AudioCapture();

    AudioCapture(const AudioCapture&)            = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    AudioHealth health() const;

private:
    struct Source;   // the open device and encoder (audio_capture.cpp)

    void run();
    bool open(Source& src);
    void setHealth(AudioState state, const std::string& device = {}, const std::string& error = {});
    void sleepFor(std::chrono::milliseconds d);

    const std::string           setting_;
    const std::string           v4l2Device_;
    const std::function<bool()> wanted_;
    const FrameCallback         onFrame_;

    mutable std::mutex healthMtx_;
    AudioHealth        health_;

    std::mutex              stopMtx_;
    std::condition_variable stopCv_;
    bool                    stopping_ = false;
    std::thread             thread_;
};

} // namespace houston_kvm
