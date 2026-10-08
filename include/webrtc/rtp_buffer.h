#pragma once

#include <rtc/rtc.hpp>

#include <array>
#include <cstdint>
#include <mutex>

namespace houston_kvm {


// store()/get() are called from different libdatachannel callback threads
// (the publisher's ingest onMessage vs. a subscriber's RTCP-NACK onMessage),
// so slot access needs its own lock rather than relying on callers.
class NackBuffer {
public:
    static constexpr size_t CAPACITY = 1024;

    void store(const rtc::binary &packet) {
        if (packet.size() < 12) return; // RTP header is at least 12 bytes
        uint16_t seq = readSeq(packet);
        size_t index = seq % CAPACITY;
        std::lock_guard<std::mutex> guard(mutex_);
        auto &slot = slots_[index];
        slot.seq = seq;
        // Copy the full packet so we can retransmit it byte for byte
        slot.data.assign(packet.begin(), packet.end());
    }

    rtc::binary get(uint16_t sequence) const {
        size_t index = sequence % CAPACITY;
        std::lock_guard<std::mutex> guard(mutex_);
        const auto &slot = slots_[index];
        if (slot.seq == sequence && !slot.data.empty()) {
            return slot.data;
        }
        return rtc::binary();
    }

private:
    static uint16_t readSeq(const rtc::binary &packet) {
        // RTP sequence number is in bytes 2 and 3 of the header
        auto packetData = reinterpret_cast<const uint8_t *>(packet.data());
        return static_cast<uint16_t>((packetData[2] << 8) | packetData[3]);
    }

    struct Slot {
        uint16_t seq = 0;
        rtc::binary data;
    };

    mutable std::mutex mutex_;
    std::array<Slot, CAPACITY> slots_;
};

} // namespace houston_kvm
