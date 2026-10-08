#include "video/audio_capture.h"

#ifdef HOUSTONKVM_AUDIO
#include <alsa/asoundlib.h>
#include <opus.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <sstream>

namespace houston_kvm {

namespace fs = std::filesystem;

namespace {

constexpr int  kRate         = 48000;          // Opus's native rate, and RTP's for it
constexpr int  kChannels     = 2;
constexpr int  kFrameSamples = kRate / 50;     // 20 ms, the usual WebRTC Opus frame
constexpr int  kBitrate      = 96000;
constexpr auto kIdlePoll     = std::chrono::milliseconds(250);
constexpr auto kMaxBackoff   = std::chrono::seconds(30);
constexpr auto kAutoCheckEvery = std::chrono::seconds(5);
// Open this long and the device counts as working: the next fault starts
// again from a 1 s retry (as for video, see stream_manager.cpp).
constexpr auto kStableAfter  = std::chrono::seconds(10);
// A device that's open but delivers nothing for this long has stopped.
constexpr auto kStalledAfter = std::chrono::seconds(2);
// Sound card and steady clock drift apart slowly; past this, re-anchor the
// timestamps to now rather than let audio wander away from video.
constexpr auto kMaxDrift     = std::chrono::milliseconds(200);

constexpr std::string_view kSyntheticPrefix = "synthetic:";

std::string readFirstLine(const fs::path& p) {
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return line;
}

// Recording devices of card `card`: the n of each /proc/asound/cardN/pcm<n>c.
std::vector<int> captureDevicesOf(int card) {
    std::vector<int> devs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/proc/asound/card" + std::to_string(card), ec)) {
        std::string n = e.path().filename().string();
        if (n.size() > 4 && n.starts_with("pcm") && n.back() == 'c') {
            try { devs.push_back(std::stoi(n.substr(3, n.size() - 4))); } catch (...) {}
        }
    }
    std::sort(devs.begin(), devs.end());
    return devs;
}

std::string alsaName(const std::string& cardId, int dev) {
    return "plughw:CARD=" + cardId + ",DEV=" + std::to_string(dev);
}

} // namespace

const char* audioStateName(AudioState state) {
    switch (state) {
        case AudioState::Off:         return "off";
        case AudioState::Unsupported: return "unsupported";
        case AudioState::Idle:        return "idle";
        case AudioState::Good:        return "good";
        case AudioState::Absent:      return "absent";
    }
    return "off";
}

bool audioSupported() {
#ifdef HOUSTONKVM_AUDIO
    return true;
#else
    return false;
#endif
}

bool isValidAudioSetting(std::string_view s) {
    if (s.empty() || s == "auto") return true;
    if (s.size() > 128) return false;
    auto restIs = [&s](size_t from, auto ok) {
        return from < s.size() && std::all_of(s.begin() + from, s.end(), ok);
    };
#ifdef HOUSTONKVM_SYNTHETIC_CAPTURE
    if (s.starts_with("synthetic:tone")) {
        return s.size() == 14 || (s[14] == '/' && restIs(15, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }));
    }
#endif
    for (std::string_view prefix : {"hw:", "plughw:", "sysdefault:"}) {
        if (s.starts_with(prefix))
            return restIs(prefix.size(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ',' ||
                       c == '=' || c == '.' || c == '-';
            });
    }
    return false;
}

std::vector<AudioDevice> enumerateAudioDevices() {
    // " 0 [Video          ]: USB-Audio - USB2 Video", then a second,
    // indented line with the long name.
    std::vector<AudioDevice> out;
    std::ifstream in("/proc/asound/cards");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        int card;
        if (!(ls >> card)) continue;
        auto open = line.find('['), close = line.find(']');
        if (open == std::string::npos || close == std::string::npos || close < open) continue;
        std::string id = line.substr(open + 1, close - open - 1);
        while (!id.empty() && id.back() == ' ') id.pop_back();
        std::string name = line.substr(close + 1);
        if (auto dash = name.find(" - "); dash != std::string::npos) name = name.substr(dash + 3);
        if (id.empty()) continue;
        auto devs = captureDevicesOf(card);
        for (int dev : devs)
            out.push_back({alsaName(id, dev), devs.size() > 1 ? name + " (device " + std::to_string(dev) + ")" : name});
    }
    return out;
}

std::string audioDeviceFor(const std::string& v4l2Device, std::string* error) {
    auto fail = [error](const char* why) {
        if (error) *error = why;
        return std::string{};
    };
    if (v4l2Device.starts_with(kSyntheticPrefix))
        return fail("A synthetic capture device has no sound card.");
    std::error_code ec;
    fs::path node = fs::canonical(v4l2Device, ec);
    if (ec) return fail("Can't find the capture device, so not its sound card either.");
    fs::path iface = fs::canonical("/sys/class/video4linux" / node.filename() / "device", ec);
    if (ec) return fail("Can't find the capture device, so not its sound card either.");
    // The video and sound functions are interfaces (1-3:1.0, 1-3:1.2) of
    // one USB device (1-3). Anything else (a PCI card, a virtual device under
    // /sys/devices/platform) would match every card that shares its bus.
    fs::path usbDevice = iface.parent_path();
    if (!fs::exists(usbDevice / "idVendor", ec))
        return fail("The capture device isn't a USB dongle, so it has no sound card of its own.");

    std::map<int, std::string> matches; // card number -> id, lowest first
    for (const auto& e : fs::directory_iterator("/sys/class/sound", ec)) {
        std::string n = e.path().filename().string();
        if (n.size() <= 4 || !n.starts_with("card") ||
            !std::all_of(n.begin() + 4, n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            continue;
        std::error_code cec;
        fs::path cardIface = fs::canonical(e.path() / "device", cec);
        if (cec || cardIface.parent_path() != usbDevice) continue;
        matches[std::stoi(n.substr(4))] = readFirstLine(e.path() / "id");
    }
    for (const auto& [card, id] : matches) {
        auto devs = captureDevicesOf(card);
        if (!devs.empty() && !id.empty()) return alsaName(id, devs.front());
    }
    return fail("The capture device has no sound card of its own.");
}

#ifdef HOUSTONKVM_AUDIO

namespace {
// alsa-lib prints its own errors to stderr; every failed open while a
// dongle is unplugged would add a few lines. We report what matters ourselves.
void quietAlsa(const char*, int, const char*, int, const char*, ...) {}
} // namespace

struct AudioCapture::Source {
    snd_pcm_t*   pcm = nullptr;
    OpusEncoder* enc = nullptr;
    bool         synthetic = false;
    double       phase = 0;               // synthetic tone only
    std::string  device;
    Clock::time_point anchor;             // when sample 0 was captured
    uint64_t     samples = 0;             // per channel, since `anchor`
    Clock::time_point lastData;

    bool isOpen() const { return enc != nullptr; }
    void close() {
        if (pcm) snd_pcm_close(pcm);
        if (enc) opus_encoder_destroy(enc);
        pcm = nullptr;
        enc = nullptr;
        synthetic = false;
    }
    ~Source() { close(); }
};

AudioCapture::AudioCapture(std::string setting, std::string v4l2Device,
                           std::function<bool()> wanted, FrameCallback onFrame)
    : setting_(std::move(setting)), v4l2Device_(std::move(v4l2Device)),
      wanted_(std::move(wanted)), onFrame_(std::move(onFrame)) {
    if (setting_.empty()) return;   // Off
    static std::once_flag quiet;
    std::call_once(quiet, [] { snd_lib_error_set_handler(quietAlsa); });
    setHealth(AudioState::Idle);
    thread_ = std::thread(&AudioCapture::run, this);
}

bool AudioCapture::open(Source& src) {
    std::string error;
    std::string dev = setting_ == "auto" ? audioDeviceFor(v4l2Device_, &error) : setting_;
    if (dev.empty()) {
        setHealth(AudioState::Absent, "", error);
        return false;
    }
    if (dev.starts_with(kSyntheticPrefix)) {
        src.synthetic = true;
    } else {
        // Non-blocking: run() waits with a timeout and checks for a stop
        // between reads, so a device that goes quiet can't hang it.
        int err = snd_pcm_open(&src.pcm, dev.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (err >= 0)
            // 100 ms of buffer; plughw converts from whatever the card does.
            err = snd_pcm_set_params(src.pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                     kChannels, kRate, 1, 100000);
        if (err < 0) {
            src.close();
            setHealth(AudioState::Absent, dev,
                      err == -EBUSY ? "The sound device is in use by something else: another target, "
                                      "or another program on this server."
                                    : std::string("Can't open the sound device: ") + snd_strerror(err));
            return false;
        }
    }
    int err = 0;
    src.enc = opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !src.enc) {
        src.enc = nullptr;
        src.close();
        setHealth(AudioState::Absent, dev, std::string("Can't start the Opus encoder: ") + opus_strerror(err));
        return false;
    }
    opus_encoder_ctl(src.enc, OPUS_SET_BITRATE(kBitrate));
    src.device   = dev;
    src.anchor   = Clock::now();
    src.samples  = 0;
    src.lastData = src.anchor;
    std::cout << "Audio: capturing from " << dev << std::endl;
    setHealth(AudioState::Good, dev);
    return true;
}

void AudioCapture::run() {
    Source src;
    auto backoff  = std::chrono::seconds(1);
    auto nextTry  = Clock::now();
    auto nextAutoCheck = Clock::now();
    std::vector<int16_t> pcm(kFrameSamples * kChannels);
    std::vector<unsigned char> packet(1500);

    auto retryLater = [&] {
        nextTry = Clock::now() + backoff;
        backoff = std::min(backoff * 2, std::chrono::duration_cast<std::chrono::seconds>(kMaxBackoff));
    };
    // A device that opens but fails again at once backs off like one that
    // won't open, rather than being reopened every second for good.
    Clock::time_point openedAt;
    auto lose = [&](const std::string& why) {
        std::cerr << "Audio: lost " << src.device << " (" << why << "); reopening" << std::endl;
        setHealth(AudioState::Absent, src.device, why);
        src.close();
        if (Clock::now() - openedAt >= kStableAfter) backoff = std::chrono::seconds(1);
        retryLater();
    };

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(stopMtx_);
            if (stopping_) return;
        }
        if (!wanted_()) {
            if (src.isOpen()) src.close();
            // The next viewer tries straight away, however long the wait had
            // grown while the last one watched.
            backoff = std::chrono::seconds(1);
            nextTry = Clock::now();
            // Nothing is opened while nobody listens, but whether "auto"
            // finds a sound card at all is a cheap look at sysfs, and
            // worth showing before anyone has to watch to find out.
            if (setting_ == "auto" && Clock::now() >= nextAutoCheck) {
                std::string error;
                std::string dev = audioDeviceFor(v4l2Device_, &error);
                if (dev.empty()) setHealth(AudioState::Absent, "", error);
                else setHealth(AudioState::Idle, dev);
                nextAutoCheck = Clock::now() + kAutoCheckEvery;
            } else if (setting_ != "auto") {
                setHealth(AudioState::Idle);
            }
            sleepFor(kIdlePoll);
            continue;
        }
        if (!src.isOpen()) {
            if (Clock::now() < nextTry) { sleepFor(kIdlePoll); continue; }
            if (!open(src)) {
                retryLater();
                continue;
            }
            openedAt = Clock::now();
        }

        // One 20 ms frame.
        if (src.synthetic) {
            auto due = src.anchor + std::chrono::nanoseconds(
                (src.samples + kFrameSamples) * 1'000'000'000ULL / kRate);
            {
                std::unique_lock<std::mutex> lock(stopMtx_);
                if (stopCv_.wait_until(lock, due, [this] { return stopping_; })) return;
            }
            for (int i = 0; i < kFrameSamples; ++i) {
                auto v = static_cast<int16_t>(std::sin(src.phase) * 6000);
                pcm[2 * i] = pcm[2 * i + 1] = v;
                src.phase += 2 * std::numbers::pi * 440.0 / kRate;
            }
            src.phase = std::fmod(src.phase, 2 * std::numbers::pi);
        } else {
            int got = 0;
            bool failed = false;
            while (got < kFrameSamples) {
                {
                    std::lock_guard<std::mutex> lock(stopMtx_);
                    if (stopping_) return;
                }
                // set_params leaves the start threshold at the whole buffer,
                // and a read smaller than it never starts a capture stream, so
                // start it ourselves: after opening, and after recovering from
                // an overrun, which leaves it prepared but stopped.
                if (snd_pcm_state(src.pcm) == SND_PCM_STATE_PREPARED) snd_pcm_start(src.pcm);
                int w = snd_pcm_wait(src.pcm, 200);
                snd_pcm_sframes_t n = w < 0 ? w : w == 0 ? 0
                    : snd_pcm_readi(src.pcm, pcm.data() + got * kChannels, kFrameSamples - got);
                if (n == -EAGAIN) n = 0;
                if (n < 0) {
                    // An overrun (we fell behind) or a suspend is recoverable;
                    // ENODEV and the like are not.
                    if (snd_pcm_recover(src.pcm, static_cast<int>(n), 1) < 0) {
                        lose(std::string("The sound device stopped: ") + snd_strerror(static_cast<int>(n)));
                        failed = true;
                        break;
                    }
                    continue;
                }
                if (n > 0) {
                    got += static_cast<int>(n);
                    src.lastData = Clock::now();
                } else if (Clock::now() - src.lastData > kStalledAfter) {
                    lose("The sound device stopped sending sound.");
                    failed = true;
                    break;
                }
            }
            if (failed) continue;
        }

        auto captured = src.anchor + std::chrono::nanoseconds(src.samples * 1'000'000'000ULL / kRate);
        src.samples += kFrameSamples;
        auto ended = captured + std::chrono::milliseconds(20);
        auto drift = Clock::now() - ended;
        if (drift > kMaxDrift || drift < -kMaxDrift) {
            src.anchor  = Clock::now() - std::chrono::milliseconds(20);
            src.samples = kFrameSamples;
            captured    = src.anchor;
        }

        opus_int32 size = opus_encode(src.enc, pcm.data(), kFrameSamples, packet.data(),
                                      static_cast<opus_int32>(packet.size()));
        if (size > 0) onFrame_(packet.data(), static_cast<size_t>(size), captured);
    }
}

#else // !HOUSTONKVM_AUDIO

struct AudioCapture::Source {};

AudioCapture::AudioCapture(std::string setting, std::string v4l2Device,
                           std::function<bool()> wanted, FrameCallback onFrame)
    : setting_(std::move(setting)), v4l2Device_(std::move(v4l2Device)),
      wanted_(std::move(wanted)), onFrame_(std::move(onFrame)) {
    if (!setting_.empty())
        setHealth(AudioState::Unsupported, "", "This build of HoustonKVM has no sound support.");
}

bool AudioCapture::open(Source&) { return false; }
void AudioCapture::run() {}

#endif

AudioCapture::~AudioCapture() {
    {
        std::lock_guard<std::mutex> lock(stopMtx_);
        stopping_ = true;
    }
    stopCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

AudioHealth AudioCapture::health() const {
    std::lock_guard<std::mutex> lock(healthMtx_);
    return health_;
}

void AudioCapture::setHealth(AudioState state, const std::string& device, const std::string& error) {
    std::lock_guard<std::mutex> lock(healthMtx_);
    if (state == AudioState::Absent && error != health_.lastError && !error.empty())
        std::cerr << "Audio: " << (device.empty() ? setting_ : device) << ": " << error << std::endl;
    health_.state = state;
    if (!device.empty()) health_.device = device;
    if (state == AudioState::Absent || state == AudioState::Unsupported) health_.lastError = error;
    else if (state == AudioState::Good) health_.lastError.clear();
}

void AudioCapture::sleepFor(std::chrono::milliseconds d) {
    std::unique_lock<std::mutex> lock(stopMtx_);
    stopCv_.wait_for(lock, d, [this] { return stopping_; });
}

} // namespace houston_kvm
