#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace houston_kvm {


// Turns a target's MJPEG frames into H.264 for WebRTC. Input is the capture
// card's JPEG as it arrived, so a backend that has hardware for it can decode
// the JPEG there as well as encode.
class VideoEncoder {
public:
    using EncodedCb = std::function<void(const uint8_t* data, size_t size)>;

    // The encoder this machine has: OpenH264 (software) for now. Throws
    // std::runtime_error if none can be set up.
    static std::unique_ptr<VideoEncoder> create(uint32_t fps, uint32_t bitrateKbps);

    virtual ~VideoEncoder() = default;

    // Which backend this is, for the log.
    virtual const char* name() const = 0;

    // Decodes one MJPEG frame and feeds it to the H.264 encoder. Malformed
    // JPEG input is silently dropped (returns without calling onNal).
    virtual void encode(const uint8_t* jpegData, size_t jpegSize, const EncodedCb& onNal) = 0;

    // Requests that the next encoded frame be an IDR (with fresh SPS/PPS).
    virtual void forceKeyframe() = 0;

protected:
    VideoEncoder() = default;
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
};

}
