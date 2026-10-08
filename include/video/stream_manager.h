#pragma once

#include "video/audio_capture.h"
#include "video/broadcaster.h"
#include "core/database.h"
#include "hid/input_backend.h"
#include "webrtc/publisher.h"
#include "webrtc/selective_forwarding_unit.h"
#include "video/v4l2_capture.h"
#include "video/video_health.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace houston_kvm {

// Owns a target's capture device, WebRTC publisher and input backend, and
// (re)builds all three from a TargetSettings, at startup and when an Owner
// changes them.
//
// applySettings() never blocks its caller: a rebuild can take ~10 s (ICE
// gathering times out at 10 s; stopping a capture takes up to ~1 s), so it
// runs on a worker thread, which also serializes overlapping calls. Sound
// (AudioCapture) is rebuilt with the rest.
//
// The same worker reopens the capture device when it stops (a dongle pulled
// out) or won't open, backing off from 1 s to 30 s, and videoHealth()
// reports meanwhile. A stopping target's worker exits before its
// replacement starts, so the two never race for the device.
//
// mutex_ guards capture_, sfuPublisher_ and input_: use withCapture() and
// withInput(), never a cached pointer, since applySettings() can swap them.
// Every frame and every input-status read takes mutex_, so nothing may wait
// on a device while holding it: withInput() is for non-blocking calls only
// (isReady(), health(), starting a self-test), and device writes go through
// inputBackend() with no lock of ours held.
class StreamManager {
public:
    StreamManager(SelectiveForwardingUnit& sfu, Broadcaster& broadcaster,
                  std::string roomId, std::string peerId);
    ~StreamManager();

    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    // Enqueues a rebuild and returns immediately. Safe to call from the uWS
    // event-loop thread.
    void applySettings(Database::TargetSettings settings);

    // How many WebRTC viewers are watching this target now.
    int webrtcViewers() const { return sfu_.openVideoSubscribers(roomId_, peerId_); }

    AudioHealth audioHealth() {
        std::lock_guard<std::mutex> lock(mutex_);
        return audio_ ? audio_->health() : AudioHealth{};
    }

    VideoHealth videoHealth() const {
        std::lock_guard<std::mutex> lock(healthMtx_);
        return videoHealth_;
    }

    template <typename F>
    auto withCapture(F&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn(capture_.get());
    }

    // Non-blocking backend calls only (see the class comment).
    template <typename F>
    auto withInput(F&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn(input_.get());
    }

    // The current backend, for device I/O outside mutex_. The reference keeps
    // a backend replaced meanwhile alive until the caller is done with it;
    // only InputQueue's worker takes one, so a replaced backend is never
    // destroyed on the event loop.
    std::shared_ptr<InputBackend> inputBackend() {
        std::lock_guard<std::mutex> lock(mutex_);
        return input_;
    }

private:
    void handleFrame(const uint8_t* data, size_t size);
    void doApplySettings(const Database::TargetSettings& settings);
    void workerLoop();
    // Worker thread only.
    bool openCapture();
    void superviseCapture();
    void restartSilentCapture(std::chrono::steady_clock::time_point now);
    void scheduleRetry(const std::string& error);
    void setVideoHealth(VideoState state, const std::string& error = {});

    SelectiveForwardingUnit& sfu_;
    Broadcaster& broadcaster_;
    std::string roomId_;
    std::string peerId_;

    std::mutex mutex_;
    std::unique_ptr<CaptureSource> capture_;
    std::unique_ptr<SfuPublisher> sfuPublisher_;
    std::shared_ptr<InputBackend> input_;
    // Declared after everything its thread's callback uses (mutex_,
    // sfuPublisher_), so it's destroyed, and its thread joined, first.
    std::unique_ptr<AudioCapture> audio_;

    // Last frame handed to the broadcaster/publisher, and when — for the
    // same-frame dedup check in handleFrame(). Only ever touched from the
    // capture thread (handleFrame is that thread's callback, and
    // doApplySettings only replaces capture_ after the old capture thread
    // has already been joined), so neither needs locking of its own.
    std::vector<uint8_t>                 lastFrame_;
    std::chrono::steady_clock::time_point lastSentTime_;

    // Capture supervision (see the class comment). Worker thread only,
    // except lastFrameNs_, which the capture thread writes.
    using Clock = std::chrono::steady_clock;
    std::string         device_;
    uint32_t            width_ = 0, height_ = 0, fps_ = 0;
    bool                lostCapture_ = false;  // retrying one that worked, not one never opened
    Clock::time_point   openedAt_;
    Clock::time_point   nextRetry_;
    std::chrono::seconds backoff_{1};
    int                 failedAttempts_ = 0;
    std::atomic<int64_t> lastFrameNs_{0};      // steady clock; 0 = none since opening
    // Open but silent: when to close and reopen it next, and how long the
    // wait after that will be (see restartSilentCapture).
    Clock::time_point   nextSilentRestart_;
    std::chrono::seconds silentBackoff_{10};
    int                 silentRestarts_ = 0;

    mutable std::mutex healthMtx_;
    VideoHealth        videoHealth_;

    std::thread              worker_;
    std::mutex               queueMtx_;
    std::condition_variable  queueCv_;
    std::deque<std::function<void()>> queue_;
    bool                     running_ = true;
};

} // namespace houston_kvm
