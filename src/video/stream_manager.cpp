#include "video/stream_manager.h"

#include "hid/ch9329_inject.h"
#include "hid/input_inject.h"
#include "hid/qemu_inject.h"
#ifdef HOUSTONKVM_SYNTHETIC_CAPTURE
#include "video/synthetic_capture.h"
#endif

#include <cstring>
#include <iostream>
#include <utility>

namespace houston_kvm {

namespace {
// Upper bound on how long a truly static picture can go without a resend —
// see the comment in handleFrame().
constexpr auto kMaxSameFrameInterval = std::chrono::seconds(2);

// How often the worker checks on the capture device when it has nothing
// else to do: cheap (a few atomic loads), and quick to notice a stop.
constexpr auto kSuperviseInterval = std::chrono::milliseconds(250);
constexpr auto kMaxBackoff = std::chrono::seconds(30);
// An open device that sends nothing for this long has no picture to send.
constexpr auto kNoSignalAfter = std::chrono::seconds(3);
// Open this long and the device counts as back for good: the next fault
// starts again from a 1 s retry, not from where the last one left off.
constexpr auto kStableAfter = std::chrono::seconds(10);
// An open device that has sent nothing this long is closed and opened
// again, then at doubling intervals up to the maximum. Some dongles stop
// streaming when their HDMI cable is pulled and don't start again by
// themselves when it's plugged back in; restarting the stream wakes them.
// The wait grows because "no picture" is also what a target that's simply
// switched off looks like, and that needs no more than an occasional look.
constexpr auto kFirstSilentRestart = std::chrono::seconds(10);
constexpr auto kMaxSilentRestart   = std::chrono::seconds(300);

int64_t steadyNs(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}
}

StreamManager::StreamManager(SelectiveForwardingUnit& sfu, Broadcaster& broadcaster,
                              std::string roomId, std::string peerId)
    : sfu_(sfu), broadcaster_(broadcaster),
      roomId_(std::move(roomId)), peerId_(std::move(peerId)) {
    worker_ = std::thread(&StreamManager::workerLoop, this);
}

StreamManager::~StreamManager() {
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        running_ = false;
    }
    queueCv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void StreamManager::applySettings(Database::TargetSettings settings) {
    std::lock_guard<std::mutex> lock(queueMtx_);
    queue_.push_back([this, settings]() { doApplySettings(settings); });
    queueCv_.notify_one();
}

void StreamManager::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMtx_);
            queueCv_.wait_for(lock, kSuperviseInterval,
                              [this] { return !running_ || !queue_.empty(); });
            if (!running_ && queue_.empty()) return;
            if (!queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (task) task();
        else superviseCapture();
    }
}

void StreamManager::handleFrame(const uint8_t* data, size_t size) {
    // Same-frame dedup, like uStreamer's --drop-same-frames: a still screen
    // isn't sent or re-encoded when its JPEG is byte-identical to the last
    // (capture chains with sensor noise never match, which is harmless).
    // Capped: a stream silent for long looks dead to browsers and proxies,
    // so a still picture is still resent at least this often.
    auto now = std::chrono::steady_clock::now();
    lastFrameNs_.store(steadyNs(now), std::memory_order_relaxed);
    bool same = size == lastFrame_.size() &&
                (size == 0 || std::memcmp(data, lastFrame_.data(), size) == 0);
    bool suppress = same && now - lastSentTime_ < kMaxSameFrameInterval;
    if (!suppress) {
        lastFrame_.assign(data, data + size);
        lastSentTime_ = now;
        broadcaster_.onFrame(data, size);
    }

    // The dedup above only applies to the MJPEG broadcast: WebRTC needs a
    // steady frame cadence to ever leave a browser's jitter buffer in a
    // "ready" state, and gates nothing correctness-wise, since H264 already
    // encodes an unchanged picture as a near-zero-size skip frame — so
    // starving the encoder here bought nothing but broke Low-Latency mode
    // on any static/idle screen (verified: encoder was dropping below
    // 2fps, and the browser's video element never left readyState 0).
    std::lock_guard<std::mutex> lock(mutex_);
    if (sfuPublisher_) sfuPublisher_->onVideoFrame(data, size);
}

void StreamManager::doApplySettings(const Database::TargetSettings& settings) {
    // 1. Tear down the old capture first — its destructor joins the capture
    // thread, guaranteeing handleFrame() can't fire with stale state while
    // we rebuild everything below. The join must happen *without* mutex_
    // held: the capture thread's loop calls handleFrame(), which locks this
    // same mutex_ (see below) — joining while holding it deadlocks against
    // that thread. Swap the pointer out under the lock, then destroy it
    // (and join) after releasing.
    std::unique_ptr<CaptureSource> oldCapture;
    std::unique_ptr<AudioCapture>  oldAudio;   // its thread locks mutex_ too
    {
        std::lock_guard<std::mutex> lock(mutex_);
        oldCapture = std::move(capture_);
        oldAudio   = std::move(audio_);
    }
    oldCapture.reset();
    oldAudio.reset();
    // The old capture thread is joined by now (see above), so this is safe
    // without a lock — don't let a reconfigure's first frame get compared
    // against a frame (or timing) from the previous device/resolution.
    lastFrame_.clear();
    lastSentTime_ = std::chrono::steady_clock::time_point{};

    // 2. Rebuild the WebRTC publisher (blocks up to ~10s on ICE gathering —
    // this is why applySettings() runs this on a worker thread, not the
    // uWS event-loop thread).
    auto newPublisher = std::make_unique<SfuPublisher>(
        sfu_, roomId_, peerId_, settings.captureFps, settings.webrtcBitrateKbps);
    sfu_.setBitrateKbps(static_cast<int>(settings.webrtcBitrateKbps));

    // 3. Rebuild the input backend. Precedence: QEMU QMP socket, then
    // CH9329 serial, then the local USB-HID-gadget fallback.
    std::unique_ptr<InputBackend> newInput;
    if (!settings.qmpSocket.empty())
        newInput = std::make_unique<QemuInject>(settings.qmpSocket);
    else if (!settings.serialDevice.empty())
        newInput = std::make_unique<Ch9329Inject>(settings.serialDevice, settings.serialBaud);
    else
        newInput = std::make_unique<InputInject>();

    // Sound is captured only while someone watches over WebRTC, like the
    // H.264 encoder runs only then (see SfuPublisher::processFrame).
    auto newAudio = std::make_unique<AudioCapture>(
        settings.audioDevice, settings.v4l2Device,
        [this] { return webrtcViewers() > 0; },
        [this](const uint8_t* opus, size_t size, AudioCapture::Clock::time_point captured) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sfuPublisher_) sfuPublisher_->onAudioFrame(opus, size, captured);
        });

    // The old backend is closed outside mutex_: closing a CH9329 joins its
    // watchdog, which can be mid-probe (see the class comment).
    std::shared_ptr<InputBackend> oldInput;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sfuPublisher_ = std::move(newPublisher);
        oldInput      = std::exchange(input_, std::move(newInput));
        audio_        = std::move(newAudio);
    }
    oldInput.reset();

    // 4. Rebuild the capture device last. Failure here is non-fatal:
    // input and the rest of the target still work, snapshot/stream return
    // 503, and superviseCapture() keeps trying to open it.
    device_ = settings.v4l2Device;
    width_  = settings.captureWidth;
    height_ = settings.captureHeight;
    fps_    = settings.captureFps;
    lostCapture_    = false;
    backoff_        = std::chrono::seconds(1);
    failedAttempts_ = 0;
    silentBackoff_  = kFirstSilentRestart;
    silentRestarts_ = 0;
    if (device_.empty()) {
        setVideoHealth(VideoState::Off);
        return;
    }
    openCapture();
}

bool StreamManager::openCapture() {
    std::unique_ptr<CaptureSource> newCapture;
    V4l2Capture::Config cfg;
    cfg.device = device_;
    cfg.width  = width_;
    cfg.height = height_;
    cfg.fps    = fps_;
    auto onFrame = [this](const uint8_t* data, size_t size) { handleFrame(data, size); };
    try {
#ifdef HOUSTONKVM_SYNTHETIC_CAPTURE
        if (SyntheticCapture::isSyntheticDevice(cfg.device))
            newCapture = std::make_unique<SyntheticCapture>(cfg.device, cfg.width, cfg.height,
                                                            cfg.fps, onFrame);
        else
#endif
            newCapture = std::make_unique<V4l2Capture>(cfg, onFrame);
    } catch (const std::exception& e) {
        scheduleRetry(e.what());
        return false;
    }

    std::cout << "V4L2: capturing from " << cfg.device
              << " @ " << newCapture->width() << "x" << newCapture->height()
              << " " << cfg.fps << "fps" << std::endl;
    openedAt_ = Clock::now();
    lastFrameNs_.store(0, std::memory_order_relaxed);
    failedAttempts_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_ = std::move(newCapture);
    }
    // Good once the first frame arrives (superviseCapture).
    setVideoHealth(lostCapture_ ? VideoState::Reconnecting : VideoState::Starting);
    return true;
}

void StreamManager::scheduleRetry(const std::string& error) {
    ++failedAttempts_;
    nextRetry_ = Clock::now() + backoff_;
    // Every attempt while the wait grows, then about every five minutes:
    // a dongle left unplugged for a week shouldn't fill the journal.
    if (backoff_ < kMaxBackoff || failedAttempts_ % 10 == 0) {
        std::cerr << error << "; trying again in " << backoff_.count() << " s"
                  << (failedAttempts_ == 1 ? " (snapshot/stream return 503 meanwhile)" : "")
                  << std::endl;
    }
    setVideoHealth(lostCapture_ ? VideoState::Reconnecting : VideoState::Absent, error);
    backoff_ = std::min(backoff_ * 2, std::chrono::duration_cast<std::chrono::seconds>(kMaxBackoff));
}

void StreamManager::superviseCapture() {
    if (device_.empty()) return;
    const auto now = Clock::now();

    std::unique_ptr<CaptureSource> stopped;
    bool have;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        have = capture_ != nullptr;
        if (have && !capture_->isRunning()) stopped = std::move(capture_);
    }
    if (stopped) {
        // Joins its thread, so not under mutex_ (see doApplySettings).
        std::string reason = stopped->stopReason();
        stopped.reset();
        lastFrame_.clear();   // its thread is joined: see doApplySettings
        lastSentTime_ = Clock::time_point{};
        lostCapture_ = true;
        nextRetry_   = now + backoff_;
        std::cerr << "V4L2: lost " << device_ << " (" << reason << "); reconnecting" << std::endl;
        setVideoHealth(VideoState::Reconnecting, reason);
        return;
    }
    if (!have) {
        if (now >= nextRetry_) openCapture();
        return;
    }

    if (now - openedAt_ >= kStableAfter) backoff_ = std::chrono::seconds(1);
    const int64_t last  = lastFrameNs_.load(std::memory_order_relaxed);
    const bool    fresh = last != 0 && steadyNs(now) - last <
        std::chrono::duration_cast<std::chrono::nanoseconds>(kNoSignalAfter).count();
    const VideoState state = videoHealth().state;
    if (fresh) {
        if (state == VideoState::Good) return;
        if (lostCapture_) {
            lostCapture_ = false;
            int n;
            {
                std::lock_guard<std::mutex> lock(healthMtx_);
                n = ++videoHealth_.reconnects;
            }
            std::cout << "V4L2: " << device_ << " is back (reconnect " << n << ")" << std::endl;
        } else if (state == VideoState::NoSignal) {
            std::cout << "V4L2: " << device_ << " has a picture again"
                      << (silentRestarts_ ? " after restarting it" : "") << std::endl;
        }
        silentBackoff_  = kFirstSilentRestart;
        silentRestarts_ = 0;
        setVideoHealth(VideoState::Good);
    } else if (state == VideoState::NoSignal) {
        if (now >= nextSilentRestart_) restartSilentCapture(now);
    } else if (now - openedAt_ >= kNoSignalAfter) {
        std::cerr << "V4L2: " << device_ << " is open but sends no picture" << std::endl;
        nextSilentRestart_ = now + silentBackoff_ - kNoSignalAfter;
        setVideoHealth(VideoState::NoSignal);
    }
}

void StreamManager::restartSilentCapture(Clock::time_point now) {
    std::unique_ptr<CaptureSource> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = std::move(capture_);
    }
    old.reset();          // joins its thread (stopping the stream), not under mutex_
    lastFrame_.clear();   // see doApplySettings
    lastSentTime_ = Clock::time_point{};
    ++silentRestarts_;
    // Every restart while the wait grows, then about every half hour.
    if (silentBackoff_ < kMaxSilentRestart || silentRestarts_ % 6 == 0)
        std::cerr << "V4L2: " << device_ << " still sends no picture; restarting it (next in "
                  << std::min(silentBackoff_ * 2, std::chrono::duration_cast<std::chrono::seconds>(
                                                      kMaxSilentRestart)).count()
                  << " s if it stays dark)" << std::endl;
    nextSilentRestart_ = now + silentBackoff_ * 2;
    silentBackoff_ = std::min(silentBackoff_ * 2,
                              std::chrono::duration_cast<std::chrono::seconds>(kMaxSilentRestart));
    // A device that went away meanwhile fails to open here and is retried
    // like any other lost one.
    if (!openCapture()) {
        lostCapture_ = true;
        setVideoHealth(VideoState::Reconnecting);
        return;
    }
    // Still no picture as far as anyone watching can tell; superviseCapture
    // says "good" again with the first frame.
    setVideoHealth(VideoState::NoSignal);
}

void StreamManager::setVideoHealth(VideoState state, const std::string& error) {
    std::lock_guard<std::mutex> lock(healthMtx_);
    videoHealth_.state  = state;
    videoHealth_.device = device_;
    if (!error.empty()) {
        std::string e = error;
        for (size_t at; !device_.empty() && (at = e.find(device_)) != std::string::npos;)
            e.replace(at, device_.size(), "the capture device");
        videoHealth_.lastError = std::move(e);
    }
    videoHealth_.retryAt = nextRetry_;
}

} // namespace houston_kvm
