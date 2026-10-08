#pragma once

#include <cstdint>
#include <vector>

namespace houston_kvm {
static constexpr uint8_t RTCP_PAYLOAD_TYPE_RTPFB = 205;   // Transport-layer FB (RFC 4585)
static constexpr uint8_t RTCP_PAYLOAD_TYPE_PSFB  = 206;   // Payload-specific FB (RFC 4585)

static constexpr uint8_t RTCP_FORMAT_NACK = 1;     // Generic NACK  (in RTPFB)
static constexpr uint8_t RTCP_FORMAT_PLI  = 1;     // Picture Loss  (in PSFB)
static constexpr uint8_t RTCP_FORMAT_FIR  = 4;     // Full Intra Request (in PSFB, RFC 5104)

struct NackEntry {
    uint16_t packetId;
    uint16_t lostPacketBitmask;
};

struct RtcpParseResult {
    std::vector<NackEntry> nacks;
    // A keyframe was asked for: a PLI, or a FIR, which is what
    // libdatachannel's Track::requestKeyframe() sends.
    bool hasPli = false;
    bool hasRemb = false; // Whether this packet contains a REMB (receiver estimated max bitrate)
    uint64_t rembBitrateBps = 0; // If hasRemb is true, the bitrate in bits per second
};

inline RtcpParseResult parseRtcp(const uint8_t *data, size_t len) {
    RtcpParseResult result;
    size_t offset = 0;

    while (offset + 4 <= len) {
        // ── RTCP common header ──────────────────────────────────
        uint8_t  firstByte = data[offset];
        uint8_t  version   = (firstByte >> 6) & 0x03;
        if (version != 2) break;                         // not RTCP v2

        uint8_t  format      = firstByte & 0x1F;           // RC or FMT
        uint8_t  payloadType = data[offset + 1];
        uint16_t lengthW     = static_cast<uint16_t>(
                                 (data[offset + 2] << 8) | data[offset + 3]);
        size_t   packetBytes = (static_cast<size_t>(lengthW) + 1) * 4;

        if (offset + packetBytes > len) break;              // truncated

        // ── Generic NACK (PT=205, FMT=1) ────────────────────────
        if (payloadType == RTCP_PAYLOAD_TYPE_RTPFB && format == RTCP_FORMAT_NACK) {
            // Fixed header = 12 bytes (common hdr 4 + sender SSRC 4 + media SSRC 4)
            // FCI starts at offset+12, each FCI entry is 4 bytes (PID + BLP)
            size_t fciStart = offset + 12;
            size_t fciEnd   = offset + packetBytes;
            for (size_t f = fciStart; f + 4 <= fciEnd; f += 4) {
                NackEntry e;
                e.packetId = static_cast<uint16_t>((data[f]     << 8) | data[f + 1]);
                e.lostPacketBitmask = static_cast<uint16_t>((data[f + 2] << 8) | data[f + 3]);
                result.nacks.push_back(e);
            }
        }

        // ── PLI (PT=206, FMT=1) or FIR (PT=206, FMT=4) ─────────
        if (payloadType == RTCP_PAYLOAD_TYPE_PSFB &&
            (format == RTCP_FORMAT_PLI || format == RTCP_FORMAT_FIR)) {
            result.hasPli = true;
        }

        // ── REMB (PT=206, FMT=15) ───────────────────────────────
        if (payloadType == RTCP_PAYLOAD_TYPE_PSFB && format == 15) {
            // Verify "REMB" magic at offset+12..15 and enough data for
            // the BR fields at offset+17..19.
            if (offset + 20 <= len &&
                data[offset + 12] == 'R' && data[offset + 13] == 'E' &&
                data[offset + 14] == 'M' && data[offset + 15] == 'B') {
                result.hasRemb = true;
                // Byte 16 = num SSRCs (skip).
                // Byte 17 = BR Exp (6 bits) | BR Mantissa high 2 bits.
                // Bytes 18-19 = BR Mantissa low 16 bits.
                uint8_t  brExp      = (data[offset + 17] >> 2) & 0x3F;
                uint32_t brMantissa = (static_cast<uint32_t>(data[offset + 17] & 0x03) << 16) |
                                      (static_cast<uint32_t>(data[offset + 18]) << 8) |
                                       static_cast<uint32_t>(data[offset + 19]);
                result.rembBitrateBps = static_cast<uint64_t>(brMantissa) << brExp;
            }
        }

        offset += packetBytes;
    }

    return result;
}

inline std::vector<uint16_t> expandNack(const NackEntry &nack) {
    std::vector<uint16_t> sequences;
    sequences.push_back(nack.packetId);
    for (int i = 0; i < 16; ++i) {
        if (nack.lostPacketBitmask & (1 << i)) {
            sequences.push_back(static_cast<uint16_t>(nack.packetId + i + 1));
        }
    }
    return sequences;
}


} // namespace houston_kvm
