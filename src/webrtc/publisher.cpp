#include "webrtc/publisher.h"

#include "webrtc/rtc_compat.h"
#include "webrtc/rtcp_parse.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>

namespace houston_kvm {

namespace {

std::string gatherLocalSdp(const std::shared_ptr<rtc::PeerConnection>& pc, rtc::Description::Type type, int timeoutSeconds = 10) {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();
    auto flag = std::make_shared<std::atomic<bool>>(false);

    pc->onGatheringStateChange([pc, promise, flag](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete && !flag->exchange(true)) {
            auto description = pc->localDescription();
            if (description) {
                description->removeIceOption("trickle");
                description->endCandidates();
            }
            promise->set_value(description ? std::string(*description) : "");
        }
    });

    try {
        pc->setLocalDescription(type);
    } catch (const std::exception& e) {
        std::cerr << "SfuPublisher: setLocalDescription failed: " << e.what() << std::endl;
        promise->set_value("");
        return "";
    }

    if (future.wait_for(std::chrono::seconds(timeoutSeconds)) == std::future_status::ready)
        return future.get();
    else {
        std::cerr << "SfuPublisher: ICE gathering timed out after " << timeoutSeconds << " seconds." << std::endl;
        return "";
    }
}

} // namespace

SfuPublisher::SfuPublisher(SelectiveForwardingUnit& sfu, std::string roomId, std::string peerId,
                            uint32_t fps, uint32_t bitrateKbps)
    : sfu_(sfu), roomId_(std::move(roomId)), peerId_(std::move(peerId)),
      fps_(fps), encoder_(VideoEncoder::create(fps, bitrateKbps)),
      startTime_(std::chrono::steady_clock::now()) {
    std::cout << "SfuPublisher [" << roomId_ << "]: H.264 encoder: " << encoder_->name() << "\n";
    connect();
    running_ = true;
    worker_ = std::thread(&SfuPublisher::workerLoop, this);
}

SfuPublisher::~SfuPublisher() {
    running_ = false;
    mailboxCv_.notify_one();
    if (worker_.joinable()) worker_.join();
    if (pc_) {
        try { pc_->close(); }
        catch (const std::exception& e) {
            std::cerr << "SfuPublisher: close failed: " << e.what() << "\n";
        }
    }
}

void SfuPublisher::connect() {
    std::string offer = sfu_.addParticipant(roomId_, peerId_);
    if (offer.empty()) {
        std::cerr << "SfuPublisher: Sfu produced no offer for [" << peerId_
                  << "] — WebRTC publishing unavailable\n";
        return;
    }
    rtc::Configuration configuration;
    configuration.disableAutoNegotiation = true;
    pc_ = std::make_shared<rtc::PeerConnection>(configuration);
    pc_->onStateChange([peerId = peerId_](rtc::PeerConnection::State s) {
        std::cout << "SfuPublisher [" << peerId << "]: " << s << "\n";
    });

    // The SFU's candidates are held back until it has our answer. With them,
    // this side starts ICE (and then DTLS) the moment it has an answer of
    // its own, and a busy machine can deliver our DTLS handshake to the SFU
    // before the answer carrying our certificate's fingerprint: libdatachannel
    // then rejects the certificate ("certificate verify failed" / "unknown
    // CA") and the link fails for good. Without them, nothing can start
    // until they are added below, after setPublishAnswer().
    std::vector<rtc::Candidate> sfuCandidates;
    try {
        rtc::Description sfuOffer(offer, "offer");
        sfuCandidates = sfuOffer.extractCandidates();
        // Not "no more candidates" any longer: they come later.
        std::string sdp = std::string(sfuOffer);
        for (size_t at; (at = sdp.find("a=end-of-candidates\r\n")) != std::string::npos;)
            sdp.erase(at, std::strlen("a=end-of-candidates\r\n"));
        pc_->setRemoteDescription(rtc::Description(sdp, "offer"));
    } catch (const std::exception& e) {
        std::cerr << "SfuPublisher: setRemoteDescription failed: " << e.what() << "\n";
        return;
    }

    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.addH264Codec(VIDEO_PT);
    video.addSSRC(VIDEO_SSRC, "video-send");
    videoTrack_ = pc_->addTrack(video);
    videoRtpConfig_ = std::make_shared<rtc::RtpPacketizationConfig>(
        VIDEO_SSRC, "video-send", VIDEO_PT, rtc_compat::kH264ClockRate);
    rtc_compat::setH264Sender(*videoTrack_, videoRtpConfig_);

    videoTrack_->onMessage(
        [this](rtc::binary message) {
            auto parsed = parseRtcp(reinterpret_cast<const uint8_t*>(message.data()), message.size());
            if (parsed.hasPli) encoder_->forceKeyframe();
        },
        nullptr);

    videoTrack_->onOpen([peerId = peerId_]() {
        std::cout << "SfuPublisher: video open [" << peerId << "]\n";
    });

    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
    audio.addOpusCodec(AUDIO_PT);
    audio.addSSRC(AUDIO_SSRC, "audio-send");
    audioTrack_ = pc_->addTrack(audio);
    audioRtpConfig_ = std::make_shared<rtc::RtpPacketizationConfig>(
        AUDIO_SSRC, "audio-send", AUDIO_PT, rtc_compat::kOpusClockRate);
    rtc_compat::setOpusSender(*audioTrack_, audioRtpConfig_);

    audioTrack_->onOpen([peerId = peerId_]() {
        std::cout << "SfuPublisher: audio open [" << peerId << "]\n";
    });

    std::string answer = gatherLocalSdp(pc_, rtc::Description::Type::Answer);
    if (answer.empty()) {
        std::cerr << "SfuPublisher: failed to produce local answer for [" << peerId_ << "]\n";
        return;
    }
    sfu_.setPublishAnswer(roomId_, peerId_, answer);
    for (const auto& candidate : sfuCandidates) {
        try {
            pc_->addRemoteCandidate(candidate);
        } catch (const std::exception& e) {
            std::cerr << "SfuPublisher: addRemoteCandidate failed: " << e.what() << "\n";
        }
    }
    std::cout << "SfuPublisher: connecting to Sfu as [" << peerId_ << "] in " << roomId_ << "\n";
}

void SfuPublisher::onVideoFrame(const uint8_t* mjpegData, size_t size) {
    {
        std::lock_guard<std::mutex> lock(mailboxMtx_);
        pendingFrame_.assign(mjpegData, mjpegData + size);
        hasPendingFrame_ = true;
    }
    mailboxCv_.notify_one();
}

void SfuPublisher::onAudioFrame(const uint8_t* opusData, size_t size,
                                std::chrono::steady_clock::time_point captured) {
    if (!audioTrack_ || !audioTrack_->isOpen()) return;

    // RTP timestamps wrap modulo 2^32, so a frame captured just before this
    // publisher started (negative here) still lands where it belongs.
    int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        captured - startTime_).count();
    uint32_t ts = static_cast<uint32_t>(elapsedUs * rtc_compat::kOpusClockRate / 1'000'000);
    try {
        rtc_compat::sendFrame(*audioTrack_, *audioRtpConfig_,
                              reinterpret_cast<const std::byte*>(opusData), size, ts);
    } catch (const std::exception& e) {
        std::cerr << "SfuPublisher: sendFrame (audio) failed: " << e.what() << "\n";
    }
}

void SfuPublisher::workerLoop() {
    while (running_) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lock(mailboxMtx_);
            mailboxCv_.wait(lock, [this] { return !running_ || hasPendingFrame_; });
            if (!running_) return;
            frame.swap(pendingFrame_);
            hasPendingFrame_ = false;
        }
        processFrame(frame.data(), frame.size());
    }
}

void SfuPublisher::processFrame(const uint8_t* mjpegData, size_t size) {
    if (encoderFailed_ || !videoTrack_ || !videoTrack_->isOpen()) return;

    // Decoding the MJPEG and encoding H.264 is most of what a target costs
    // (about a quarter of a core at 720p30), so do neither while nobody
    // watches over WebRTC. The first viewer starts from a keyframe.
    int viewers = sfu_.openVideoSubscribers(roomId_, peerId_);
    if (viewers == 0) {
        if (encoding_)
            std::cout << "SfuPublisher [" << roomId_ << "]: no WebRTC viewers, encoder idle" << std::endl;
        encoding_ = false;
        return;
    }
    if (!encoding_) {
        std::cout << "SfuPublisher [" << roomId_ << "]: WebRTC viewer watching, encoding" << std::endl;
        encoder_->forceKeyframe();
        encodeMsSum_ = 0;
        encodeMsMax_ = 0;
        encodeSampleCount_ = 0;
        encoding_ = true;
    }

    auto start = std::chrono::steady_clock::now();

    // An exception escaping this worker thread would terminate the whole
    // server, taking MJPEG and input down with WebRTC.
    try {
        encoder_->encode(mjpegData, size, [this, start](const uint8_t* nal, size_t nalSize) {
            double elapsedUs = std::chrono::duration<double, std::micro>(
                start - startTime_).count();
            uint32_t ts = static_cast<uint32_t>(
                elapsedUs * rtc_compat::kH264ClockRate / 1'000'000.0);
            try {
                rtc_compat::sendFrame(*videoTrack_, *videoRtpConfig_,
                                      reinterpret_cast<const std::byte*>(nal), nalSize, ts);
            } catch (const std::exception& e) {
                std::cerr << "SfuPublisher: sendFrame failed: " << e.what() << "\n";
            }
        });
    } catch (const std::exception& e) {
        encoderFailed_ = true;
        std::cerr << "SfuPublisher [" << peerId_ << "]: " << e.what()
                  << " — low-latency (WebRTC) video disabled; MJPEG is unaffected."
                     " If only the noopenh264 stub is installed, install Cisco's"
                     " openh264 (the epel-cisco-openh264 / fedora-cisco-openh264"
                     " repo) and restart.\n";
        return;
    }

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    encodeMsSum_ += ms;
    encodeMsMax_ = std::max(encodeMsMax_, ms);
    if (++encodeSampleCount_ >= static_cast<int>(fps_)) {
        std::cout << "SfuPublisher: encode+send avg="
                   << (encodeMsSum_ / encodeSampleCount_) << "ms max=" << encodeMsMax_
                   << "ms over " << encodeSampleCount_ << " frames\n";
        encodeMsSum_ = 0;
        encodeMsMax_ = 0;
        encodeSampleCount_ = 0;
    }
}

} // namespace houston_kvm
