#pragma once

// Builds against both libdatachannel APIs HoustonKVM meets in practice: the
// current one (0.20 and later, which the RPM bundles on EL10) and 0.19, which
// is what EPEL 9 ships as libdatachannel-devel. Using the system library
// there is what lets the EL9 package stop bundling its own copy.
//
// 0.20 replaced the packetization handlers with chainable packetizers and
// added Track::sendFrame(FrameInfo); <rtc/frameinfo.hpp> arrived with it, so
// its presence tells the two apart without a version macro (0.19 has none).

#include <rtc/rtc.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#if __has_include(<rtc/frameinfo.hpp>)
#define HOUSTONKVM_RTC_CHAINED_PACKETIZERS 1
#else
#define HOUSTONKVM_RTC_CHAINED_PACKETIZERS 0
#endif

namespace rtc_compat {

// Fixed by the RTP payload formats (RFC 6184 for H.264, RFC 7587 for Opus);
// the libraries' own constants for them are named differently per version.
inline constexpr uint32_t kH264ClockRate = 90000;
inline constexpr uint32_t kOpusClockRate = 48000;

#if !HOUSTONKVM_RTC_CHAINED_PACKETIZERS
// 0.19's RtcpSrReporter only sends a Sender Report when asked, and stamps it
// with rtpConfig->timestamp rather than the packet it follows. 0.20 does both
// by itself once a second; this element sits in front of the reporter and
// does the same, so browsers can keep audio and video in sync either way.
class SrPacer final : public rtc::MediaHandlerElement {
public:
    SrPacer(std::shared_ptr<rtc::RtpPacketizationConfig> config,
            std::shared_ptr<rtc::RtcpSrReporter> reporter)
        : config_(std::move(config)), reporter_(std::move(reporter)) {}

    rtc::ChainedOutgoingProduct processOutgoingBinaryMessage(rtc::ChainedMessagesProduct messages,
                                                             rtc::message_ptr control) override {
        if (messages && !messages->empty()) {
            const auto& last = messages->back();
            if (last->size() >= sizeof(rtc::RtpHeader))
                config_->timestamp = reinterpret_cast<const rtc::RtpHeader*>(last->data())->timestamp();
        }
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport_ >= std::chrono::seconds(1)) {
            reporter_->setNeedsToReport();
            lastReport_ = now;
        }
        return {messages, control};
    }

private:
    std::shared_ptr<rtc::RtpPacketizationConfig> config_;
    std::shared_ptr<rtc::RtcpSrReporter> reporter_;
    std::chrono::steady_clock::time_point lastReport_{};
};

template <class Handler>
void attachChain(rtc::Track& track, std::shared_ptr<Handler> handler,
                 const std::shared_ptr<rtc::RtpPacketizationConfig>& config) {
    auto reporter = std::make_shared<rtc::RtcpSrReporter>(config);
    handler->addToChain(std::make_shared<SrPacer>(config, reporter));
    handler->addToChain(reporter);
    track.setMediaHandler(handler);
}
#endif

// Packetize H.264 access units (Annex B start codes) sent with sendFrame(),
// followed by RTCP Sender Reports.
inline void setH264Sender(rtc::Track& track, const std::shared_ptr<rtc::RtpPacketizationConfig>& config) {
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, config);
#if HOUSTONKVM_RTC_CHAINED_PACKETIZERS
    track.setMediaHandler(packetizer);
    track.chainMediaHandler(std::make_shared<rtc::RtcpSrReporter>(config));
#else
    attachChain(
        track, std::make_shared<rtc::H264PacketizationHandler>(packetizer), config);
#endif
}

// Packetize Opus frames sent with sendFrame(), followed by RTCP Sender Reports.
inline void setOpusSender(rtc::Track& track, const std::shared_ptr<rtc::RtpPacketizationConfig>& config) {
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(config);
#if HOUSTONKVM_RTC_CHAINED_PACKETIZERS
    track.setMediaHandler(packetizer);
    track.chainMediaHandler(std::make_shared<rtc::RtcpSrReporter>(config));
#else
    attachChain(
        track, std::make_shared<rtc::OpusPacketizationHandler>(packetizer), config);
#endif
}

// For a track that forwards already-packetized RTP: RTCP Sender Reports only.
inline void setRelaySender(rtc::Track& track, const std::shared_ptr<rtc::RtpPacketizationConfig>& config) {
#if HOUSTONKVM_RTC_CHAINED_PACKETIZERS
    track.setMediaHandler(std::make_shared<rtc::RtcpSrReporter>(config));
#else
    attachChain(
        track,
        std::make_shared<rtc::MediaChainableHandler>(std::make_shared<rtc::MediaHandlerRootElement>()),
        config);
#endif
}

// Send one frame stamped with an RTP timestamp in the track's clock rate.
inline void sendFrame(rtc::Track& track, rtc::RtpPacketizationConfig& config,
                      const std::byte* data, size_t size, uint32_t timestamp) {
#if HOUSTONKVM_RTC_CHAINED_PACKETIZERS
    (void)config;
    track.sendFrame(data, size, rtc::FrameInfo(timestamp));
#else
    config.timestamp = timestamp;
    track.send(data, size);
#endif
}

} // namespace rtc_compat
