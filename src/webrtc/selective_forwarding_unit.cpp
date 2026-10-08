#include "webrtc/selective_forwarding_unit.h"

#include "webrtc/rtc_compat.h"

#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <sstream>
#include <tuple>

namespace houston_kvm {

static bool tryClose(const std::shared_ptr<rtc::PeerConnection>& peerConnection) noexcept {
    if (!peerConnection) return true; // already closed
    try {
        peerConnection->close(); return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to close PeerConnection" << std::endl;
        std::cerr << "Error: " << e.what() << std::endl;
        return false;
    }
}

SelectiveForwardingUnit::SelectiveForwardingUnit(int bitrateKbps, std::vector<IceServerConfig> iceServers)
: bitrateKbps_(bitrateKbps),
    iceServers_(std::move(iceServers)),
    running_(std::make_shared<std::atomic<bool>>(true)) {
    housekeepingThread_ = std::thread(&SelectiveForwardingUnit::housekeepingLoop, this);
}

SelectiveForwardingUnit::~SelectiveForwardingUnit() {
    *running_ = false;
    if (housekeepingThread_.joinable()) {
        housekeepingThread_.join();
    }
    // Taken out under the lock, closed outside it: closing can fire a
    // connection's state callback on this thread, and the callback takes the
    // lock itself (EDEADLK, which leaves the connection half closed and its
    // ICE thread never stopped, so destroying it would wait forever).
    decltype(rooms_) rooms;
    {
        std::unique_lock lock(mutex_);
        rooms.swap(rooms_);
    }
    for (auto &[roomId, room] : rooms) {
        for (auto &[peerId, p] : room->participants) {
            for (auto &[subscriberId, sub] : p->subscribers) {
                tryClose(sub->peerConnection);
            }
            tryClose(p->publishPeerConnection);
        }
    }
}

static bool trySend(const std::shared_ptr<rtc::Track>& track, const rtc::binary& message) noexcept {
    try {
        track->send(message); return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to send message on track: " << track->mid() << std::endl;
        std::cerr << "Error: " << e.what() << std::endl;
        return false;
    }
}

static bool tryRequestKeyframe(const std::shared_ptr<rtc::Track>& track,
                                std::atomic<int64_t>& lastKeyframeReqNs) noexcept {
    if (!track || !track->isOpen()) return false;
    try {
        track->requestKeyframe();
        lastKeyframeReqNs.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to send keyframe request: " << e.what() << std::endl;
        return false;
    }
}

// Keyframes are asked for only when needed: a viewer's video opening, or a
// viewer reporting loss (PLI). A keyframe every second would be the quality
// "pulse" video_encoder.cpp avoids.
void SelectiveForwardingUnit::housekeepingLoop() {
    while (running_->load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        reapSubscriptions();
    }
}

void SelectiveForwardingUnit::deliverOffer(Subscriber& sub, std::string sdp) {
    if (sub.offerSent.exchange(true)) return;
    // Only the one caller that got here touches onOffer.
    auto onOffer = std::move(sub.onOffer);
    sub.onOffer  = nullptr;
    if (onOffer) onOffer(std::move(sdp));
}

bool SelectiveForwardingUnit::finishGathering(Subscriber& sub, rtc::PeerConnection& pc,
                                              const std::string& subscriberId,
                                              const std::string& publisherId, bool settle) {
    auto description = pc.localDescription();
    if (settle && (!description || description->candidates().empty())) return false;
    auto note = [&](size_t count) {
        if (settle)
            std::cerr << "SFU: sub [" << subscriberId << "] going ahead with " << count
                      << " ICE candidate(s): gathering is still waiting on a STUN/TURN server"
                         " that doesn't answer (Admin -> Network, or --ice-server)\n";
    };
    if (sub.deferCandidates) {
        // The offer went out already; the candidates go with the answer.
        // Marked gathered under the same lock that stores them, which is
        // the lock setSubscribeAnswer() checks it under.
        CandidatesCallback waiting;
        std::vector<rtc::Candidate> candidates;
        {
            std::lock_guard candidatesLock(sub.candidatesMutex);
            if (sub.gathered) return true;
            if (description) sub.candidates = description->candidates();
            sub.gathered = true;
            candidates = sub.candidates;
            waiting    = std::move(sub.onCandidates);
            sub.onCandidates = nullptr;
        }
        note(candidates.size());
        if (waiting) waiting(std::move(candidates));
        return true;
    }
    if (sub.gathered.exchange(true)) return true;
    std::string offer;
    if (description) {
        note(description->candidates().size());
        description->removeIceOption("trickle");
        description->endCandidates();
        offer = description->generateSdp();
    }
    std::cout << "SFU: sub offer [" << subscriberId << "<-" << publisherId
              << "] (" << offer.size() << " B)\n";
    deliverOffer(sub, std::move(offer));
    return true;
}

void SelectiveForwardingUnit::reapSubscriptions() {
    std::vector<std::pair<std::string, std::shared_ptr<Subscriber>>> expired;
    // (subscriberId, publisherId, subscription)
    std::vector<std::tuple<std::string, std::string, std::shared_ptr<Subscriber>>> slow;
    {
        std::unique_lock lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto &[roomId, room] : rooms_) {
            for (auto &[peerId, p] : room->participants) {
                for (auto it = p->subscribers.begin(); it != p->subscribers.end();) {
                    auto& sub = it->second;
                    auto age  = now - sub->createdAt;
                    bool gatherExpired  = !sub->gathered && age > kSubscriptionGatherTimeout;
                    bool connectExpired = !sub->connected && age > kSubscriptionConnectTimeout;
                    if (gatherExpired || connectExpired) {
                        expired.emplace_back(it->first, sub);
                        it = p->subscribers.erase(it);
                    } else {
                        if (!sub->gathered && age > kSubscriptionGatherSettle)
                            slow.emplace_back(it->first, peerId, sub);
                        ++it;
                    }
                }
            }
        }
    }
    // Outside the lock, like closing below: the connection takes its own
    // locks, and its callbacks take this one. A subscription with no
    // candidates yet stays for kSubscriptionGatherTimeout to decide.
    for (auto &[subscriberId, publisherId, sub] : slow)
        finishGathering(*sub, *sub->peerConnection, subscriberId, publisherId, true);
    // Outside the lock: closing fires the connection's state callback,
    // which takes the lock itself.
    for (auto &[subscriberId, sub] : expired) {
        std::cerr << "SFU: sub [" << subscriberId << "] "
                  << (sub->gathered ? "never connected" : "ICE gathering timed out")
                  << " — closed\n";
        deliverOffer(*sub, "");
        CandidatesCallback waiting;
        {
            std::lock_guard candidatesLock(sub->candidatesMutex);
            waiting = std::move(sub->onCandidates);
            sub->onCandidates = nullptr;
        }
        if (waiting) waiting(std::nullopt);
        tryClose(sub->peerConnection);
    }
}

int SelectiveForwardingUnit::openVideoSubscribers(const std::string& roomId,
                                                  const std::string& publisherId) const {
    std::shared_lock lock(mutex_);
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) return 0;
    auto partIt = roomIt->second->participants.find(publisherId);
    if (partIt == roomIt->second->participants.end()) return 0;
    const int64_t now     = std::chrono::steady_clock::now().time_since_epoch().count();
    const int64_t silence = std::chrono::nanoseconds(kSubscriberSilenceTimeout).count();
    int watching = 0;
    for (auto &[subscriberId, sub] : partIt->second->subscribers)
        if (sub->videoTrack && sub->videoTrack->isOpen() &&
            now - sub->lastHeardNs.load(std::memory_order_relaxed) < silence)
            ++watching;
    return watching;
}

void SelectiveForwardingUnit::handleSubscriberRtcp(const std::shared_ptr<Participant>& publisher, const std::shared_ptr<Subscriber>& sub, const rtc::binary& rtcp) {
    auto parsed = parseRtcp(reinterpret_cast<const uint8_t*>(rtcp.data()), rtcp.size());
    for (auto &nack : parsed.nacks) {
        auto sequences = expandNack(nack);
        for (uint16_t seq : sequences) {
            rtc::binary packet = publisher->videoNackBuffer.get(seq);
            if (!packet.empty() && sub->videoTrack && sub->videoTrack->isOpen())
                trySend(sub->videoTrack, packet);
        }
    }

    if (parsed.hasPli) {
        tryRequestKeyframe(publisher->publishVideo, publisher->lastKeyframeReqNs);
    }

    if (parsed.hasRemb && parsed.rembBitrateBps > 0) {
        sub->lastRembBps.store(parsed.rembBitrateBps,
                               std::memory_order_relaxed);

        // Use the MAX REMB across all subscribers so that one
        // slow subscriber doesn't throttle the publisher too much.
        // publisher->subscribers is mutated (insert/erase) under a unique
        // lock elsewhere (createSubscription, removeParticipant, the
        // egress-connection close handler), so iterating it here needs at
        // least a shared lock to avoid racing those mutations.
        uint64_t maxRembBps = 0;
        {
            std::shared_lock lock(mutex_);
            for (auto &[subscriberId, s] : publisher->subscribers) {
                uint64_t sRembBps = s->lastRembBps.load(std::memory_order_relaxed);
                if (sRembBps > maxRembBps) maxRembBps = sRembBps;
            }
        }

        if (maxRembBps > 0 && publisher->publishVideo && publisher->publishVideo->isOpen()) {
            uint64_t floorBps = static_cast<uint64_t>(bitrateKbps_.load(std::memory_order_relaxed)) * 1000;
            if (maxRembBps < floorBps) maxRembBps = floorBps; // don't go below the configured bitrate

            uint64_t prev = publisher->lastRembBps.load(std::memory_order_relaxed);
            uint64_t delta = (maxRembBps > prev) ? (maxRembBps - prev) : (prev - maxRembBps);
            if (prev == 0 || delta > (prev / 10)) { // only send if changed by more than 10%
                publisher->lastRembBps.store(maxRembBps, std::memory_order_relaxed);
                forwardRembToPublisher(publisher, maxRembBps);
            }
        }
    }
}


void SelectiveForwardingUnit::forwardRembToPublisher(const std::shared_ptr<Participant>& publisher, uint64_t bitrateBps) {
    uint8_t exp = 0;
    uint64_t mantissa = bitrateBps;
    while (mantissa > 0x3FFFF) {
        mantissa >>= 1;
        exp++;
    }
    uint8_t remb[24] = {
        0x8F, 0xCE, 0x00, 0x06, // RTCP header: FMT=15, PT=206, length=6
        'R', 'E', 'M', 'B',     // "REMB" ASCII marker
        0x01,                   // SSRC count = 1
        exp,                    // exponent
        static_cast<uint8_t>((mantissa >> 16) & 0xFF),// mantissa high byte
        static_cast<uint8_t>((mantissa >> 8) & 0xFF), // mantissa mid byte
        static_cast<uint8_t>(mantissa & 0xFF),       // mantissa low byte
        static_cast<uint8_t>((VIDEO_SSRC >> 24) & 0xFF),
        static_cast<uint8_t>((VIDEO_SSRC >> 16) & 0xFF),
        static_cast<uint8_t>((VIDEO_SSRC >> 8) & 0xFF),
        static_cast<uint8_t>(VIDEO_SSRC & 0xFF)      // SSRC of the video track
    };

    auto rembBytes = reinterpret_cast<const std::byte*>(remb);
    rtc::binary rembPacket(rembBytes, rembBytes + sizeof(remb));
    if (publisher->publishVideo && publisher->publishVideo->isOpen()) {
        trySend(publisher->publishVideo, rembPacket);
    }
}

std::string SelectiveForwardingUnit::gatherSdp(const std::shared_ptr<rtc::PeerConnection>& peerConnection, int timeoutSeconds) {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();
    auto flag = std::make_shared<std::atomic<bool>>(false);

    peerConnection->onGatheringStateChange([promise, flag, peerConnection](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete && !flag->exchange(true)) {
            auto description = peerConnection->localDescription();
            if (description) {
                description->removeIceOption("trickle");
                description->endCandidates();
            }
            promise->set_value(description ? description->generateSdp() : "");
        }
    });

    try {
        peerConnection->setLocalDescription();
    } catch (const std::exception& e) {
        std::cerr << "Failed to set local description: " << e.what() << std::endl;
        promise->set_value("");
        return "";
    }

    if (future.wait_for(std::chrono::seconds(timeoutSeconds)) == std::future_status::timeout) {
        std::cerr << "SDP gathering timed out after " << timeoutSeconds << " seconds." << std::endl;
        return "";
    }
    return future.get();
}

static int extractVideoPayloadType(const std::string &sdp, int fallback = 96) {
    std::istringstream stream(sdp);
    std::string line;
    bool inVideo = false;
    int h264PayloadType = -1, vp8PayloadType = -1;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("m=video", 0) == 0) { inVideo = true; continue; }
        if (inVideo && line.rfind("m=", 0) == 0) break;
        if (inVideo && line.rfind("a=rtpmap:", 0) == 0) {
            auto colonPos = line.find(':');
            auto spacePos = line.find(' ', colonPos);
            if (colonPos == std::string::npos || spacePos == std::string::npos) continue;
            int payloadType = std::stoi(line.substr(colonPos + 1, spacePos - colonPos - 1));
            std::string codec = line.substr(spacePos + 1);
            for (auto &c : codec) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (codec.find("h264") != std::string::npos) h264PayloadType = payloadType;
            else if (codec.find("vp8") != std::string::npos) vp8PayloadType = payloadType;
        }
    }

    if (h264PayloadType >= 0) return h264PayloadType;
    if (vp8PayloadType >= 0) return vp8PayloadType;
    return fallback;
}

void SelectiveForwardingUnit::setIceServers(std::vector<IceServerConfig> servers) {
    std::lock_guard lock(iceMutex_);
    iceServers_ = std::move(servers);
}

std::vector<IceServerConfig> SelectiveForwardingUnit::iceServers() const {
    std::lock_guard lock(iceMutex_);
    return iceServers_;
}

rtc::Configuration SelectiveForwardingUnit::makeIceConfig(bool withIceServers) const {
    rtc::Configuration config;
    config.portRangeBegin = kWebrtcPortRangeBegin;
    config.portRangeEnd = kWebrtcPortRangeEnd;
    if (!withIceServers) return config;
    for (const auto& iceServer : iceServers()) {
        rtc::IceServer server(iceServer.url);
        if (!iceServer.username.empty()) {
            server.username = iceServer.username;
            server.password = iceServer.credential;
        }
        config.iceServers.push_back(std::move(server));
    }
    return config;
}

std::string SelectiveForwardingUnit::addParticipant(const std::string &roomId,
                                 const std::string &peerId) {

    std::unique_lock lock(mutex_);
    auto &room = rooms_[roomId];
    if (!room) {
        room = std::make_shared<Room>();
    }

    room->participants.erase(peerId); // remove any existing participant with the same peerId

    auto participant = std::make_shared<Participant>();
    participant->peerId = peerId;

    rtc::Configuration config = makeIceConfig(false);
    participant->publishPeerConnection = std::make_shared<rtc::PeerConnection>(config);
    participant->publishPeerConnection->onLocalCandidate([this, peerId](const rtc::Candidate& c) {
        if (publisherCandidateCallback_)
            publisherCandidateCallback_(peerId, std::string(c), c.mid());
    });

    participant->publishPeerConnection->onStateChange([peerId](rtc::PeerConnection::State s) {
        std::cout << "SFU pub [" << peerId << "]: " << s << "\n";
    });

    rtc::Description::Video video("video",
                                 rtc::Description::Direction::RecvOnly);
    video.addH264Codec(96);
    video.addVP8Codec(97);
    video.setBitrate(bitrateKbps_.load(std::memory_order_relaxed));

    participant->publishVideo = participant->publishPeerConnection->addTrack(video);
    participant->publishVideo->setMediaHandler(
        std::make_shared<rtc::RtcpReceivingSession>());

    participant->publishVideo->onMessage(
        [this, roomId, peerId](rtc::binary message) {
            if (message.size() < 12) return; // RTP header is at least 12 bytes
            auto rtp = reinterpret_cast<rtc::RtpHeader *>(message.data());
            rtp->setSsrc(VIDEO_SSRC);

            std::shared_lock lock(mutex_);
            auto roomIt = rooms_.find(roomId);
            if (roomIt == rooms_.end()) return;
            auto partIt = roomIt->second->participants.find(peerId);
            if (partIt == roomIt->second->participants.end()) return;

            auto &publisher = partIt->second;
            publisher->videoNackBuffer.store(message);
            for (auto &[subscriberId, sub] : publisher->subscribers) {
                if (sub->videoTrack && sub->videoTrack->isOpen()) {
                    trySend(sub->videoTrack, message);
                }
            }
        },
        nullptr);

    participant->publishVideo->onOpen([peerId]() {
        std::cout << "SFU: pub video open [" << peerId << "]\n";
    });

    rtc::Description::Audio audio("audio",
                                 rtc::Description::Direction::RecvOnly);
    audio.addOpusCodec(111);
    participant->publishAudio = participant->publishPeerConnection->addTrack(audio);

    participant->publishAudio->onMessage(
        [this, roomId, peerId](rtc::binary message) {
            if (message.size() < 12) return; // RTP header is at least 12 bytes
            auto rtp = reinterpret_cast<rtc::RtpHeader *>(message.data());
            rtp->setSsrc(AUDIO_SSRC);

            std::shared_lock lock(mutex_);
            auto roomIt = rooms_.find(roomId);
            if (roomIt == rooms_.end()) return;
            auto partIt = roomIt->second->participants.find(peerId);
            if (partIt == roomIt->second->participants.end()) return;

            auto &publisher = partIt->second;
            publisher->audioNackBuffer.store(message);
            for (auto &[subscriberId, sub] : publisher->subscribers) {
                if (sub->audioTrack && sub->audioTrack->isOpen()) {
                    trySend(sub->audioTrack, message);
                }
            }
        },
        nullptr);

    participant->publishAudio->onOpen([peerId]() {
        std::cout << "SFU: pub audio open [" << peerId << "]\n";
    });

    room->participants[peerId] = participant;

    auto peerConnection = participant->publishPeerConnection;
    lock.unlock();

    std::string offerSdp = gatherSdp(peerConnection);
    if (offerSdp.empty()) {
        std::cerr << "Failed to gather SDP for participant: " << peerId << std::endl;
        return "";
    }
    return offerSdp;
}

void SelectiveForwardingUnit::setPublishAnswer(const std::string &roomId,
                                                const std::string &peerId,
                                                const std::string &sdp) {
    std::shared_lock lock(mutex_);
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) return;
    auto partIt = roomIt->second->participants.find(peerId);
    if (partIt == roomIt->second->participants.end()) return;

    rtc::Description answer(sdp, "answer");
    try {
        partIt->second->publishPeerConnection->setRemoteDescription(answer);
    } catch (const std::exception& e) {
        std::cerr << "SFU: setRemoteDescription failed [" << peerId << "]: " << e.what() << "\n";
        return;
    }
    std::cout << "SFU: pub answer [" << peerId << "]\n";
}

void SelectiveForwardingUnit::createSubscription(const std::string &roomId,
                                                  const std::string &subscriberId,
                                                  int64_t ownerId,
                                                  const std::string &publisherId,
                                                  bool deferCandidates,
                                                  OfferCallback onOffer) {
    std::unique_lock lock(mutex_);
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        std::cerr << "Room not found: " << roomId << std::endl;
        lock.unlock();
        onOffer("");
        return;
    }

    auto participantIt = roomIt->second->participants.find(publisherId);
    if (participantIt == roomIt->second->participants.end()) {
        std::cerr << "Publisher not found: " << publisherId << std::endl;
        lock.unlock();
        onOffer("");
        return;
    }

    auto sub = std::make_shared<Subscriber>();
    sub->ownerId   = ownerId;
    sub->createdAt = std::chrono::steady_clock::now();
    sub->onOffer   = std::move(onOffer);
    sub->deferCandidates = deferCandidates;
    std::weak_ptr<Subscriber> weakSelf = sub;
    rtc::Configuration config = makeIceConfig();
    sub->peerConnection = std::make_shared<rtc::PeerConnection>(config);

    sub->peerConnection->onLocalCandidate([this, subscriberId](const rtc::Candidate& c) {
        if (subscriberCandidateCallback_)
            subscriberCandidateCallback_(subscriberId, std::string(c), c.mid());
    });

    sub->peerConnection->onStateChange([this, roomId, publisherId, subscriberId, weakSelf](rtc::PeerConnection::State s) {
        std::cout << "SFU sub [" << subscriberId << "]: " << s << "\n";
        if (s == rtc::PeerConnection::State::Connected) {
            if (auto self = weakSelf.lock()) self->connected = true;
        }
        if (s == rtc::PeerConnection::State::Closed || s == rtc::PeerConnection::State::Failed) {
            std::unique_lock lock(mutex_);
            auto roomIt = rooms_.find(roomId);
            if (roomIt != rooms_.end()) {
                auto participantIt = roomIt->second->participants.find(publisherId);
                if (participantIt != roomIt->second->participants.end()) {
                    participantIt->second->subscribers.erase(subscriberId);
                }
            }
        }
    });

    std::weak_ptr<rtc::Track> weakPubVideo = participantIt->second->publishVideo;
    std::weak_ptr<Participant> weakPublisher = participantIt->second;

    int videoPayloadType = 96;
    if (participantIt->second->publishVideo && participantIt->second->publishPeerConnection) {
        auto localDesc = participantIt->second->publishPeerConnection->localDescription();
        if (localDesc) {
            videoPayloadType = extractVideoPayloadType(localDesc->generateSdp());
        }
    }

    rtc::Description::Video video("video",
                                 rtc::Description::Direction::SendOnly);
    video.addH264Codec(videoPayloadType);
    video.setBitrate(bitrateKbps_.load(std::memory_order_relaxed));
    // Video and audio get separate msids on purpose: browsers lip-sync the
    // tracks of one stream, holding video back to match audio's deeper
    // jitter buffer (~400 ms in Chrome), and cursor latency matters more
    // here than lip sync. The UI builds its own MediaStream from both.
    video.addSSRC(VIDEO_SSRC, "video-send", "houstonkvm", "video-send");
    sub->videoTrack = sub->peerConnection->addTrack(video);

    auto videoRtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        VIDEO_SSRC, "video-send", videoPayloadType, 90000);
    rtc_compat::setRelaySender(*sub->videoTrack, videoRtpConfig);

    std::weak_ptr<Subscriber> weakSub = sub;

    sub->videoTrack->onMessage(
        [this, weakPublisher, weakPubVideo, weakSub](const rtc::binary& message) {
            auto pub = weakPublisher.lock();
            auto s   = weakSub.lock();
            if (!pub || !s) return;
            s->lastHeardNs.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                 std::memory_order_relaxed);
            handleSubscriberRtcp(pub, s, message);
        },
        nullptr);

    sub->videoTrack->onOpen(
        [this, subscriberId, publisherId, weakPubVideo, weakPublisher, weakSub]() {
            std::cout << "SFU: sub video open [" << subscriberId
                      << "<-" << publisherId << "]\n";
            // Counts as heard from: the first RTCP is up to a second away.
            if (auto s = weakSub.lock())
                s->lastHeardNs.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                     std::memory_order_relaxed);
        if (auto pub = weakPublisher.lock()) {
            pub->lastKeyframeReqNs.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_relaxed);
            if (auto t = weakPubVideo.lock())
                tryRequestKeyframe(t, pub->lastKeyframeReqNs);
        }

            std::thread([weakVideo = weakPubVideo, weakPub = weakPublisher,
                         running = running_]() {
                for (int i = 0; i < 20; ++i) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(100));
                    if (!running->load(std::memory_order_relaxed)) return;
                }
                auto pub = weakPub.lock();
                auto t   = weakVideo.lock();
                if (pub && t)
                    tryRequestKeyframe(t, pub->lastKeyframeReqNs);
            }).detach();
        });

    rtc::Description::Audio audio("audio",
                                 rtc::Description::Direction::SendOnly);
    audio.addOpusCodec(111);
    audio.addSSRC(AUDIO_SSRC, "audio-send", "houstonkvm-audio", "audio-send");

    sub->audioTrack = sub->peerConnection->addTrack(audio);

    auto audioRtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        AUDIO_SSRC, "audio-send", 111, 48000);
    rtc_compat::setRelaySender(*sub->audioTrack, audioRtpConfig);

    sub->audioTrack->onMessage(
        [this, weakPublisher, weakSub](rtc::binary message) {
            auto pub = weakPublisher.lock();
            auto s   = weakSub.lock();
            if (!pub || !s) return;
            s->lastHeardNs.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                 std::memory_order_relaxed);
            auto parsed = parseRtcp(
                reinterpret_cast<const uint8_t *>(message.data()),
                message.size());
            for (auto &nack : parsed.nacks) {
                auto seqs = expandNack(nack);
                for (uint16_t seq : seqs) {
                    rtc::binary packet = pub->audioNackBuffer.get(seq);
                    if (!packet.empty() && s->audioTrack && s->audioTrack->isOpen())
                        trySend(s->audioTrack, packet);
                }
            }
        },
        nullptr);
    sub->audioTrack->onOpen(
        [subscriberId, publisherId]() {
            std::cout << "SFU: sub audio open [" << subscriberId
                      << "<-" << publisherId << "]\n";
        });

    participantIt->second->subscribers[subscriberId] = sub;

    auto peerConnection = sub->peerConnection;
    lock.unlock();

    // Weak references: the connection owns this callback, and the
    // subscription owns the connection.
    std::weak_ptr<rtc::PeerConnection> weakPc = peerConnection;
    auto offerIfGathered = [weakSelf, weakPc, subscriberId, publisherId]() {
        auto self = weakSelf.lock();
        auto pc   = weakPc.lock();
        if (!self || !pc || pc->gatheringState() != rtc::PeerConnection::GatheringState::Complete)
            return;
        finishGathering(*self, *pc, subscriberId, publisherId, false);
    };
    peerConnection->onGatheringStateChange(
        [offerIfGathered](rtc::PeerConnection::GatheringState) { offerIfGathered(); });
    try {
        peerConnection->setLocalDescription();
    } catch (const std::exception& e) {
        std::cerr << "SFU: setLocalDescription failed [" << subscriberId << "]: " << e.what() << "\n";
        // Left for reapSubscriptions() to remove, like any other that
        // never connects.
        deliverOffer(*sub, "");
        return;
    }
    if (deferCandidates) {
        // Offered straight away, with whatever candidates gathered so far
        // taken out; gathering carries on while the viewer answers.
        std::string offer;
        if (auto description = peerConnection->localDescription()) {
            description->extractCandidates();
            offer = description->generateSdp();
        }
        std::cout << "SFU: sub offer [" << subscriberId << "<-" << publisherId
                  << "] (" << offer.size() << " B, candidates on answer)\n";
        deliverOffer(*sub, std::move(offer));
    }
    // Gathering may have finished before the callback above was set.
    offerIfGathered();
}
    

bool SelectiveForwardingUnit::setSubscribeAnswer(const std::string &roomId,
                              const std::string &subscriberId,
                              int64_t ownerId,
                              const std::string &publisherId,
                              const std::string &sdp,
                              CandidatesCallback onCandidates) {
    std::shared_lock lock(mutex_);
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) return false;
    auto partIt = roomIt->second->participants.find(publisherId);
    if (partIt == roomIt->second->participants.end()) return false;
    auto subIt = partIt->second->subscribers.find(subscriberId);
    if (subIt == partIt->second->subscribers.end()) return false;
    if (subIt->second->ownerId != ownerId) {
        std::cerr << "SFU: setSubscribeAnswer owner mismatch [" << subscriberId << "]\n";
        return false;
    }

    rtc::Description answer(sdp, "answer");
    try {
        subIt->second->peerConnection->setRemoteDescription(answer);
    } catch (const std::exception& e) {
        std::cerr << "SFU: setRemoteDescription failed [" << subscriberId << "<-" << publisherId << "]: " << e.what() << "\n";
        throw;
    }
    std::cout << "SFU: sub answer [" << subscriberId
              << "<-" << publisherId << "]\n";

    // Only now, with the answer applied, may the viewer have our candidates.
    std::shared_ptr<Subscriber> sub = subIt->second;
    lock.unlock();
    if (!sub->deferCandidates) {
        onCandidates(std::vector<rtc::Candidate>{});
        return true;
    }
    std::vector<rtc::Candidate> candidates;
    {
        std::lock_guard candidatesLock(sub->candidatesMutex);
        if (!sub->gathered) {
            sub->onCandidates = std::move(onCandidates);
            return true;
        }
        candidates = sub->candidates;
    }
    onCandidates(std::move(candidates));
    return true;
}

// Adds one remote ICE candidate to a subscriber's egress connection. Only
// meaningful for trickle signaling, which nothing in this project
// currently uses.
void SelectiveForwardingUnit::addSubscribeIce(const std::string &roomId,
                            const std::string &subscriberId,
                            int64_t ownerId,
                            const std::string &publisherId,
                            const std::string &candidate,
                            const std::string &mid) {
    std::shared_lock lock(mutex_);
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) return;
    auto partIt = roomIt->second->participants.find(publisherId);
    if (partIt == roomIt->second->participants.end()) return;
    auto subIt = partIt->second->subscribers.find(subscriberId);
    if (subIt == partIt->second->subscribers.end()) return;
    if (subIt->second->ownerId != ownerId) {
        std::cerr << "SFU: addSubscribeIce owner mismatch [" << subscriberId << "]\n";
        return;
    }

    try {
        subIt->second->peerConnection->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception& e) {
        std::cerr << "SFU: addRemoteCandidate failed [" << subscriberId << "]: " << e.what() << "\n";
    }
}

void SelectiveForwardingUnit::removeParticipant(const std::string &roomId,
                              const std::string &peerId) {
    // Closed after the lock is released, as in the destructor; holding the
    // participant and subscriptions until then keeps their connections from
    // being destroyed under the lock too.
    std::shared_ptr<Participant> removed;
    std::vector<std::shared_ptr<Subscriber>> removedSubs;
    {
        std::unique_lock lock(mutex_);

        auto roomIt = rooms_.find(roomId);
        if (roomIt == rooms_.end()) return;
        auto &room = roomIt->second;

        auto partIt = room->participants.find(peerId);
        if (partIt != room->participants.end()) {
            removed = partIt->second;
            room->participants.erase(partIt);
        }

        for (auto &[otherPeerId, p] : room->participants) {
            auto subIt = p->subscribers.find(peerId);
            if (subIt != p->subscribers.end()) {
                removedSubs.push_back(subIt->second);
                p->subscribers.erase(subIt);
            }
        }

        if (room->participants.empty())
            rooms_.erase(roomIt);
    }

    if (removed) {
        for (auto &[subscriberId, sub] : removed->subscribers)
            tryClose(sub->peerConnection);
        tryClose(removed->publishPeerConnection);
    }
    for (auto &sub : removedSubs)
        tryClose(sub->peerConnection);

    std::cout << "SFU: removed " << peerId << " from " << roomId << "\n";
}



} // namespace houston_kvm
