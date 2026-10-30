#pragma once

#include "webrtc/selective_forwarding_unit.h"
#include "video/video_encoder.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace houston_kvm {
class SfuPublisher {
public:
    SfuPublisher(SelectiveForwardingUnit& sfu, std::string roomId, std::string peerId,
                 uint32_t fps, uint32_t bitrateKbps);
    ~SfuPublisher();

    SfuPublisher(const SfuPublisher&) = delete;
    SfuPublisher& operator=(const SfuPublisher&) = delete;
    void onVideoFrame(const uint8_t* mjpegData, size_t size);

    // Sends one Opus frame (AudioCapture), stamped with when it was
    // captured, on the same clock as video so browsers keep the two in sync.
    void onAudioFrame(const uint8_t* opusData, size_t size,
                      std::chrono::steady_clock::time_point captured);

private:
    static constexpr rtc::SSRC VIDEO_SSRC = 100; // Arbitrary: the SFU rewrites it on ingest
    static constexpr int VIDEO_PT = 96;
    static constexpr rtc::SSRC AUDIO_SSRC = 101; // Arbitrary: the SFU rewrites it on ingest
    static constexpr int AUDIO_PT = 111;         // Must match SelectiveForwardingUnit::addParticipant's Opus pt
    void connect();
    void workerLoop();
    void processFrame(const uint8_t* mjpegData, size_t size);

    SelectiveForwardingUnit& sfu_;
    std::string roomId_;
    std::string peerId_;
    uint32_t    fps_;

    std::unique_ptr<VideoEncoder> encoder_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track>          videoTrack_;
    std::shared_ptr<rtc::Track>          audioTrack_;
    std::shared_ptr<rtc::RtpPacketizationConfig> videoRtpConfig_;
    std::shared_ptr<rtc::RtpPacketizationConfig> audioRtpConfig_;

    std::chrono::steady_clock::time_point startTime_;
    std::thread             worker_;
    std::atomic<bool>       running_{false};
    std::mutex              mailboxMtx_;
    std::condition_variable mailboxCv_;
    std::vector<uint8_t>    pendingFrame_;
    bool                    hasPendingFrame_ = false;
    // Set once the H.264 encoder fails to start (e.g. only the noopenh264
    // stub is installed). Only touched on the worker thread. The publisher
    // is rebuilt on every settings change, which is when a retry could help.
    bool                    encoderFailed_   = false;
    // Whether the last frame was encoded: nothing is decoded or encoded
    // while no WebRTC viewer is watching. Only touched on the worker thread.
    bool                    encoding_        = false;
    // Whether the worker wants frames. It clears this when one finds nothing
    // to encode it for (no viewer, or no working encoder or link), so
    // onVideoFrame() stops copying them over, and sets it again when its
    // check every kIdleCheck finds a viewer. Only the worker asks the SFU:
    // onVideoFrame() runs under StreamManager's lock, which input takes
    // too, and the SFU's lock can be held for a while. (A viewer's keyframe
    // request can't wake it: the SFU can address one only to a stream it
    // has had packets from.)
    std::atomic<bool>       wanted_{true};
    // So the first viewer waits at most this long for encoding to start;
    // each check is a few microseconds.
    static constexpr auto   kIdleCheck = std::chrono::milliseconds(25);
    double  encodeMsSum_       = 0;
    double  encodeMsMax_       = 0;
    int     encodeSampleCount_ = 0;
};

}
