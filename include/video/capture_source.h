#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace houston_kvm {

// Where a target's MJPEG frames come from. V4l2Capture is the real one;
// SyntheticCapture (only in builds with HOUSTONKVM_SYNTHETIC_CAPTURE) stands
// in for a capture card so load tests can run many targets on one machine.
class CaptureSource {
public:
    /// Invoked on the source's own thread for every new MJPEG frame.
    using FrameCallback = std::function<void(const uint8_t* data, size_t size)>;

    virtual ~CaptureSource() = default;

    /// A copy of the most recent frame, or empty if none has arrived yet.
    virtual std::vector<uint8_t> snapshot() const = 0;

    /// What the source actually produces, which may differ from what was
    /// asked for (a driver can round a resolution or frame rate).
    virtual uint32_t width() const noexcept  = 0;
    virtual uint32_t height() const noexcept = 0;
    virtual uint32_t fps() const noexcept    = 0;

    /// False once the source has stopped by itself (the device went away);
    /// StreamManager then closes it and opens the device again.
    virtual bool isRunning() const noexcept = 0;

    /// Why it stopped, once isRunning() is false ("VIDIOC_DQBUF: No such
    /// device").
    virtual std::string stopReason() const = 0;
};

} // namespace houston_kvm
