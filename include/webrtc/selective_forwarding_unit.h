#pragma once

#include <rtc/rtc.hpp>

#include "webrtc/ice_servers.h"
#include "webrtc/rtcp_parse.h"
#include "webrtc/rtp_buffer.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace houston_kvm {

// Fixed SSRCs the server stamps on everything it forwards, so subscribers
// see a stable identity regardless of what a publisher's encoder emitted.
constexpr uint32_t VIDEO_SSRC = 0xFACEB00C;
constexpr uint32_t AUDIO_SSRC = 0xFACEB00D;

constexpr uint16_t kWebrtcPortRangeBegin = 50000;
constexpr uint16_t kWebrtcPortRangeEnd = 50100;

// (peerId, candidate, mid)
using CandidateCallback =
    std::function<void(const std::string&, const std::string&, const std::string&)>;

// Receives a subscription's SDP offer, or "" if there is none to give.
using OfferCallback = std::function<void(std::string sdp)>;

// Receives the server's ICE candidates for a subscription once its answer is
// in, or nullopt if they can't be had (gathering failed or timed out).
using CandidatesCallback = std::function<void(std::optional<std::vector<rtc::Candidate>>)>;

// How long a subscription may take to gather its ICE candidates before it
// is given up (its OfferCallback gets ""), and how long it may take after
// that to connect before it is closed. An abandoned subscription (a viewer
// who never answers, a closed tab) otherwise keeps its PeerConnection and a
// UDP port from kWebrtcPortRange* for good.
constexpr auto kSubscriptionGatherTimeout  = std::chrono::seconds(5);
constexpr auto kSubscriptionConnectTimeout = std::chrono::seconds(30);
// ICE gathering normally finishes in well under a second. One still going
// after this is waiting on a STUN or TURN server that doesn't answer: the
// subscription goes ahead with the candidates it has (the server's own
// addresses, enough on a LAN) rather than failing at
// kSubscriptionGatherTimeout. Only one with no candidates at all fails. The
// browser does the same on its side (ICE_GATHER_MAX_MS in ui/js/webrtc.js),
// at the same time, so this adds no wait of its own there.
constexpr auto kSubscriptionGatherSettle   = std::chrono::seconds(2);
// A viewer's browser sends RTCP about once a second while it receives. One
// that has been silent this long counts as gone for openVideoSubscribers(),
// long before ICE gives up on the connection: a closed tab or a dropped
// network says nothing.
constexpr auto kSubscriberSilenceTimeout = std::chrono::seconds(5);

// One egress connection: roomId+publisherId's video/audio relayed to a
// single subscriberId.
struct Subscriber {
    std::shared_ptr<rtc::PeerConnection> peerConnection;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;
    std::atomic<uint64_t> lastRembBps{0};
    // The user id that created this subscription (via createSubscription's
    // ownerId param) — setSubscribeAnswer() checks this so one logged-in
    // user can't apply an SDP answer to a subscription they didn't create,
    // even though subscriberId itself is unguessable (128 random bits).
    int64_t ownerId = -1;

    std::chrono::steady_clock::time_point createdAt;
    std::atomic<bool> connected{false};
    // steady_clock nanoseconds when the viewer was last heard from (its
    // video track opening, or any RTCP from it).
    std::atomic<int64_t> lastHeardNs{0};
    // Called once, by whichever comes first: ICE gathering completing (or
    // settling, see kSubscriptionGatherSettle; or, with deferCandidates, the
    // offer being made), or the SFU giving up on it. offerSent says which
    // already has.
    OfferCallback     onOffer;
    std::atomic<bool> offerSent{false};

    // See createSubscription(). Once gathered, the candidates wait here for
    // the answer, or the answer waits in onCandidates for them; both are
    // guarded by candidatesMutex.
    bool                        deferCandidates = false;
    std::atomic<bool>           gathered{false};
    std::mutex                  candidatesMutex;
    std::vector<rtc::Candidate> candidates;
    CandidatesCallback          onCandidates;
};

// One publisher's ingest connection plus everyone currently subscribed to
// it.
struct Participant {
    std::string peerId;

    std::shared_ptr<rtc::PeerConnection> publishPeerConnection;
    std::shared_ptr<rtc::Track> publishVideo;
    std::shared_ptr<rtc::Track> publishAudio;

    std::unordered_map<std::string, std::shared_ptr<Subscriber>> subscribers;

    NackBuffer videoNackBuffer;
    NackBuffer audioNackBuffer;

    std::atomic<int64_t> lastKeyframeReqNs{0};
    std::atomic<uint64_t> lastRembBps{0};
};

struct Room {
    std::unordered_map<std::string, std::shared_ptr<Participant>> participants;
};

// Minimal SFU: one ingest PeerConnection per publisher, one egress
// PeerConnection per (publisher, subscriber) pair, RTP fanned out as-is
// (no decode/re-encode), NACK/PLI/REMB handled per subscriber.
class SelectiveForwardingUnit {
public:
    SelectiveForwardingUnit(int bitrateKbps, std::vector<IceServerConfig> iceServers);
    ~SelectiveForwardingUnit();

    // The STUN/TURN servers for connections to viewers. Changing them
    // affects subscriptions made from then on; connected viewers keep theirs.
    void setIceServers(std::vector<IceServerConfig> servers);
    std::vector<IceServerConfig> iceServers() const;

    SelectiveForwardingUnit(const SelectiveForwardingUnit&) = delete;
    SelectiveForwardingUnit& operator=(const SelectiveForwardingUnit&) = delete;

    // Creates (replacing any existing one) peerId's ingest PeerConnection
    // and returns the resulting SDP offer, or "" on failure.
    std::string addParticipant(const std::string& roomId, const std::string& peerId);

    // Creates an egress PeerConnection relaying publisherId's media to
    // subscriberId, and returns at once: ICE gathering, which can take
    // seconds (a STUN server that doesn't answer), must never hold up the
    // caller, which is usually the event loop. onOffer is called exactly
    // once, on another thread (or on this one, if it fails straight away),
    // with the SDP offer, or "" if publisherId isn't in the room or
    // gathering fails or outlasts kSubscriptionGatherTimeout. ownerId is
    // recorded on the Subscriber and checked by setSubscribeAnswer().
    //
    // deferCandidates: the offer carries none of the server's ICE
    // candidates and comes at once, without waiting for gathering; they are
    // handed over by setSubscribeAnswer() instead, after the answer is
    // applied. A viewer that has them earlier starts ICE, then DTLS, before
    // the server has its answer, and libdatachannel can reject the viewer's
    // certificate for good: PeerConnection::setRemoteDescription() lets ICE
    // connect before it stores the description the fingerprint check reads.
    void createSubscription(const std::string& roomId,
                            const std::string& subscriberId,
                            int64_t ownerId,
                            const std::string& publisherId,
                            bool deferCandidates,
                            OfferCallback onOffer);

    // Applies a publisher's SDP answer to its ingest connection.
    void setPublishAnswer(const std::string& roomId,
                           const std::string& peerId,
                           const std::string& sdp);

    // Applies a subscriber's SDP answer to its egress connection. Returns
    // false if there is no such subscription (never made, or already given
    // up on) or ownerId isn't the user who created it: the same answer
    // either way, so it gives no signal about whether subscriberId exists
    // for someone else's session. Throws on SDP that can't be applied.
    // Once the answer is applied, onCandidates gets the server's candidates
    // (none, for a subscription that sent them with its offer): at once if
    // gathering is done, else when it is, possibly on another thread.
    bool setSubscribeAnswer(const std::string& roomId,
                             const std::string& subscriberId,
                             int64_t ownerId,
                             const std::string& publisherId,
                             const std::string& sdp,
                             CandidatesCallback onCandidates);

    // Adds one remote ICE candidate to a subscriber's egress connection.
    // No-ops if ownerId doesn't match the user who created the
    // subscription, mirroring setSubscribeAnswer().
    void addSubscribeIce(const std::string& roomId,
                          const std::string& subscriberId,
                          int64_t ownerId,
                          const std::string& publisherId,
                          const std::string& candidate,
                          const std::string& mid);

    // Closes and removes peerId (as publisher and/or subscriber) from
    // roomId.
    void removeParticipant(const std::string& roomId, const std::string& peerId);

    void onPublisherCandidate(CandidateCallback callback) { publisherCandidateCallback_ = std::move(callback); }
    void onSubscriberCandidate(CandidateCallback callback) { subscriberCandidateCallback_ = std::move(callback); }

    // How many of publisherId's subscribers are watching now: video track
    // open, and heard from within kSubscriberSilenceTimeout. Safe to call
    // from any thread.
    int openVideoSubscribers(const std::string& roomId, const std::string& publisherId) const;

    // Updates the target bitrate used for new SDP negotiations and as the
    // REMB floor. Safe to call from any thread.
    void setBitrateKbps(int kbps) { bitrateKbps_.store(kbps, std::memory_order_relaxed); }

private:
    // Four times a second: settles gathering that outlasts
    // kSubscriptionGatherSettle, and gives up on subscriptions that outlast
    // kSubscriptionGatherTimeout or kSubscriptionConnectTimeout.
    void housekeepingLoop();
    void reapSubscriptions();
    static void deliverOffer(Subscriber& sub, std::string sdp);
    // Hands the subscription's ICE candidates over: in the offer, or (with
    // deferCandidates) to the answer. All of them when gathering is
    // complete; with `settle`, the ones gathered so far, and false (nothing
    // done) while there are none. Once per subscription, whoever calls first.
    static bool finishGathering(Subscriber& sub, rtc::PeerConnection& pc,
                                const std::string& subscriberId, const std::string& publisherId,
                                bool settle);

    void handleSubscriberRtcp(const std::shared_ptr<Participant>& publisher,
                               const std::shared_ptr<Subscriber>& sub,
                               const rtc::binary& rtcp);
    void forwardRembToPublisher(const std::shared_ptr<Participant>& publisher,
                                 uint64_t bitrateBps);

    // STUN/TURN servers only for connections to viewers: a target's
    // publisher is on this machine, reached by its own addresses.
    rtc::Configuration makeIceConfig(bool withIceServers = true) const;
    std::string gatherSdp(const std::shared_ptr<rtc::PeerConnection>& peerConnection,
                           int timeoutSeconds = 5);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;

    std::atomic<int> bitrateKbps_;
    // Its own lock: makeIceConfig() runs under mutex_.
    mutable std::mutex           iceMutex_;
    std::vector<IceServerConfig> iceServers_;

    std::shared_ptr<std::atomic<bool>> running_;
    std::thread housekeepingThread_;

    CandidateCallback publisherCandidateCallback_;
    CandidateCallback subscriberCandidateCallback_;
};

} // namespace houston_kvm
