#pragma once

#include "video/capture_source.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace houston_kvm {

// A stand-in for a capture card, for load testing: plays a short loop of
// frames at the configured size and rate. The frames are JPEG-encoded once,
// up front, so like a real capture card it costs the server nothing per
// frame and everything measured is HoustonKVM's own work (fan-out, H.264
// encoding, WebRTC).
//
// Selected by a target device of "synthetic:<pattern>", optionally followed
// by "/<name>" (e.g. synthetic:desktop/3), since no two targets may share a
// device:
//   synthetic:desktop  mostly still screen of text with a moving pointer
//                      and a changing clock, like an idle desktop
//   synthetic:motion   every pixel changes every frame, like playing video;
//                      the worst case for the H.264 encoder
//
// "?unplugged=<file>" at the end (synthetic:desktop/3?unplugged=/tmp/x)
// makes it behave like a dongle that is pulled out while <file> exists:
// opening it fails the way opening a missing /dev/videoN does, and a
// running source stops delivering frames as soon as the file appears, the
// way V4l2Capture stops on ENODEV. "nosignal=<file>" keeps it open but
// sends no frames while <file> exists, like a dongle whose HDMI cable is
// out. "stall=<file>" is the less forgiving dongle: once <file> appears it
// stops sending and stays stopped, even after <file> is gone, until it is
// closed and opened again. Options combine with "&". Tests use them to
// exercise recovery without real hardware.
class SyntheticCapture final : public CaptureSource {
public:
    static constexpr std::string_view kPrefix = "synthetic:";

    static bool isSyntheticDevice(std::string_view device) {
        return device.substr(0, kPrefix.size()) == kPrefix;
    }

    /// Throws std::invalid_argument for an unknown pattern, and
    /// V4l2Exception (CannotOpen) while the unplugged file exists.
    SyntheticCapture(std::string_view device, uint32_t width, uint32_t height,
                     uint32_t fps, FrameCallback onFrame);
    ~SyntheticCapture() override;

    SyntheticCapture(const SyntheticCapture&)            = delete;
    SyntheticCapture& operator=(const SyntheticCapture&) = delete;

    std::vector<uint8_t> snapshot() const override;
    uint32_t width() const noexcept override  { return width_; }
    uint32_t height() const noexcept override { return height_; }
    uint32_t fps() const noexcept override    { return fps_; }
    bool isRunning() const noexcept override  { return running_.load(std::memory_order_acquire); }
    std::string stopReason() const override   { return stopReason_; }

private:
    void run();

    uint32_t      width_;
    uint32_t      height_;
    uint32_t      fps_;
    std::string   unpluggedFile_;   // empty: never unplugged
    std::string   noSignalFile_;    // empty: always a signal
    std::string   stallFile_;       // empty: never stalls
    bool          stalled_ = false; // latched: see "stall=" above
    FrameCallback frameCb_;

    std::vector<std::vector<uint8_t>> frames_;
    size_t                            latest_ = 0;
    mutable std::mutex                latestMtx_;

    std::atomic<bool> running_{true};
    std::string       stopReason_;   // see V4l2Capture::stopReason_
    std::thread       thread_;
};

} // namespace houston_kvm
