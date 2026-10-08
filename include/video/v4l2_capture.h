#pragma once

#include "video/capture_source.h"

#include <linux/videodev2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace houston_kvm {


enum class V4l2Error {
    CannotOpen,
    NotCaptureDevice,
    NoStreamingSupport,
    MjpegUnsupported,
    RequestBuffersFailed,
    InsufficientBuffers,
    MmapFailed,
    StreamingStartFailed,
};

class V4l2Exception : public std::runtime_error {
public:
    explicit V4l2Exception(const std::string& msg, V4l2Error code)
        : std::runtime_error(msg), code_(code) {}
    V4l2Error code() const noexcept { return code_; }
private:
    V4l2Error code_;
};

class V4l2Capture final : public CaptureSource {
public:

    struct Config {
        std::string device   = "/dev/video0";
        uint32_t    width    = 1280;
        uint32_t    height   = 720;
        uint32_t    fps      = 30;
        uint32_t    numBufs  = 4;   ///< Number of mmap buffers to allocate
    };

    /// One MJPEG resolution the device reports via VIDIOC_ENUM_FRAMESIZES,
    /// with the discrete frame rates it reports for that resolution via
    /// VIDIOC_ENUM_FRAMEINTERVALS (empty if the driver didn't report any
    /// discrete intervals — e.g. a continuous/stepwise range).
    struct FrameSize {
        uint32_t              width;
        uint32_t              height;
        std::vector<uint32_t> fps;
    };

    /// Queries the modes a device supports for MJPEG capture without
    /// starting a capture session — used to drive the settings UI's
    /// resolution/fps picker so it only ever offers combinations the
    /// hardware actually reports, rather than letting the caller guess.
    /// Safe to call while a V4l2Capture is already streaming from a
    /// *different* fd on the same device (VIDIOC_ENUM_* ioctls don't
    /// require exclusive access). Only reports V4L2_FRMSIZE_TYPE_DISCRETE
    /// sizes; a driver that only advertises a continuous/stepwise range
    /// yields an empty result, same as any other query failure.
    /// Throws V4l2Exception on open/VIDIOC_QUERYCAP failure, same as the
    /// constructor.
    static std::vector<FrameSize> enumerateModes(const std::string& device);

    /// One /dev/video* node that's usable as a capture device.
    struct DeviceInfo {
        std::string path;          ///< e.g. "/dev/video1"
        std::string name;          ///< v4l2_capability.card — the driver's
                                    ///< human-readable name for the hardware
                                    ///< (e.g. a capture card's product name),
                                    ///< so a picker can show something more
                                    ///< useful than a bare device number.
        bool        mjpegCapable;  ///< A live VIDIOC_ENUM_FRAMESIZES check
                                    ///< for MJPEG, same test as
                                    ///< enumerateModes() returning non-empty.
                                    ///< Not a hard filter: a card can fail
                                    ///< this transiently (no signal yet) and
                                    ///< still be the right device to pick,
                                    ///< so callers should flag rather than
                                    ///< hide a device with this false.
    };

    /// Scans /dev for video* nodes and reports the ones that are V4L2
    /// capture devices (V4L2_CAP_VIDEO_CAPTURE) — used to drive the Admin
    /// page's capture-device picker so setup doesn't require already
    /// knowing which of possibly several video nodes a capture card
    /// exposes (e.g. a raw node alongside the MJPEG-capable one, as seen
    /// on some UVC dongles) is the right one. Never throws — a node that
    /// can't be opened or queried is silently skipped rather than failing
    /// the whole scan.
    static std::vector<DeviceInfo> enumerateDevices();

    /// Opens the device, negotiates MJPEG format, starts the capture thread.
    /// Throws std::runtime_error on any failure.
    explicit V4l2Capture(Config cfg, FrameCallback onFrame);

    /// Signals the capture thread to stop, then joins it and releases resources.
    ~V4l2Capture() override;

    V4l2Capture(const V4l2Capture&)            = delete;
    V4l2Capture& operator=(const V4l2Capture&) = delete;

    /// Returns a copy of the most recently captured MJPEG frame, or an empty
    /// vector if no frame has been received yet.
    std::vector<uint8_t> snapshot() const override;

    bool isRunning() const noexcept override {
        return running_.load(std::memory_order_acquire);
    }
    // Written before running_ goes false, and read only after, so it
    // needs no lock of its own.
    std::string stopReason() const override { return stopReason_; }

    /// Actual negotiated capture resolution, which the driver is free to
    /// adjust away from Config::width/height if the exact size isn't
    /// supported for the current input signal — always check this rather
    /// than assuming the requested size was honored.
    uint32_t width()  const noexcept override { return cfg_.width; }
    uint32_t height() const noexcept override { return cfg_.height; }

    /// Actual negotiated frame rate (VIDIOC_G_PARM readback after the
    /// best-effort VIDIOC_S_PARM in openDevice()) — the driver can round or
    /// ignore the requested rate, so this can differ from Config::fps.
    uint32_t fps() const noexcept override { return cfg_.fps; }

private:
    void openDevice();
    void initMmap();
    void startStreaming();
    void stopStreaming();
    void captureLoop();
    void stopWithError(const std::string& reason);
    // Unmaps buffers and closes fd_, if open. Used by both the destructor
    // and the constructor's failure path, since a throwing constructor never
    // runs ~V4l2Capture() — anything opened before the failure would
    // otherwise leak.
    void releaseResources() noexcept;

    Config        cfg_;
    FrameCallback frameCb_;
    int           fd_ = -1;

    struct MmapBuffer {
        void*  start  = nullptr;
        size_t length = 0;
    };
    std::vector<MmapBuffer> bufs_;

    std::atomic<bool> running_{false};
    std::string       stopReason_;
    std::thread       captureThread_;

    mutable std::mutex       snapshotMtx_;
    std::vector<uint8_t>     latestFrame_;
};

}
