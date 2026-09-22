/// @module RtpH264
/// @also RtspDriver

/// The RTP packetisation an RTSP session ships H.264 over. A decoder accepts or rejects a packet on its header bits alone, so that shape is pinned here rather than by watching a player fail.

#include "doctest.h"
#include "light/util/RtpH264.h"

#include <cstring>
#include <vector>

namespace {

/// One captured datagram, as the sink saw it.
struct Packet {
    std::vector<uint8_t> bytes;
    bool     marker() const { return (bytes[1] & 0x80) != 0; }
    uint16_t seq() const { return static_cast<uint16_t>((bytes[2] << 8) | bytes[3]); }
    uint32_t ts() const {
        return (static_cast<uint32_t>(bytes[4]) << 24) | (static_cast<uint32_t>(bytes[5]) << 16) |
               (static_cast<uint32_t>(bytes[6]) << 8) | bytes[7];
    }
    const uint8_t* payload() const { return bytes.data() + mm::rtp::kHeaderBytes; }
    size_t payloadLen() const { return bytes.size() - mm::rtp::kHeaderBytes; }
};

struct Capture {
    std::vector<Packet> packets;
    size_t refuseAfter = SIZE_MAX;   // the sink starts refusing once this many are written
};

bool capture(void* ctx, const uint8_t* p, size_t len) {
    auto* c = static_cast<Capture*>(ctx);
    if (c->packets.size() >= c->refuseAfter) return false;
    c->packets.push_back(Packet{std::vector<uint8_t>(p, p + len)});
    return true;
}

/// An Annex B access unit: each NAL prefixed by a 4-byte start code.
std::vector<uint8_t> annexB(const std::vector<std::vector<uint8_t>>& nals) {
    std::vector<uint8_t> out;
    for (const auto& n : nals) {
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
}

}  // namespace

// A NAL that fits the MTU rides alone, payload byte-for-byte: the commonest case, and the one a decoder handles without reassembly.
TEST_CASE("RtpH264 sends a small NAL as one packet, payload unchanged") {
    std::vector<uint8_t> nal{0x65, 0xAA, 0xBB, 0xCC};       // an IDR slice, four bytes
    const auto au = annexB({nal});
    uint8_t scratch[mm::rtp::kMaxPacketBytes];
    Capture cap;
    mm::rtp::Packetiser p(0x1234ABCD, 7);

    const size_t n = p.writeAccessUnit(au.data(), au.size(), 900, scratch, sizeof(scratch), capture, &cap);
    REQUIRE(n == 1);
    REQUIRE(cap.packets.size() == 1);
    CHECK(cap.packets[0].payloadLen() == nal.size());
    CHECK(std::memcmp(cap.packets[0].payload(), nal.data(), nal.size()) == 0);
    CHECK(cap.packets[0].seq() == 7);
    CHECK(cap.packets[0].ts() == 900);
}

// The marker names the END of an access unit, and a timestamp says when a frame is DISPLAYED, so every packet of one frame shares one.
TEST_CASE("RtpH264 marks the last packet of an access unit and no other") {
    const auto au = annexB({{0x67, 0x01}, {0x68, 0x02}, {0x65, 0x03, 0x04}});   // SPS, PPS, IDR
    uint8_t scratch[mm::rtp::kMaxPacketBytes];
    Capture cap;
    mm::rtp::Packetiser p(1, 0);

    const size_t n = p.writeAccessUnit(au.data(), au.size(), 4500, scratch, sizeof(scratch), capture, &cap);
    REQUIRE(n == 3);
    CHECK_FALSE(cap.packets[0].marker());
    CHECK_FALSE(cap.packets[1].marker());
    CHECK(cap.packets[2].marker());
    for (const auto& q : cap.packets) CHECK(q.ts() == 4500);
}

// An oversized NAL fragments into FU-A, and a decoder rebuilds it from those alone, so the pieces must reassemble byte for byte.
TEST_CASE("RtpH264 fragments an oversized NAL and the pieces reassemble") {
    std::vector<uint8_t> nal(4000);
    nal[0] = 0x65;                                   // an IDR, so nri and type are both non-zero
    for (size_t i = 1; i < nal.size(); i++) nal[i] = static_cast<uint8_t>(i & 0xFF);
    const auto au = annexB({nal});
    uint8_t scratch[mm::rtp::kMaxPacketBytes];
    Capture cap;
    mm::rtp::Packetiser p(2, 100);

    const size_t n = p.writeAccessUnit(au.data(), au.size(), 90, scratch, sizeof(scratch), capture, &cap);
    REQUIRE(n > 1);                                  // it did fragment

    // Start and end bits land on the first and last fragment, and nowhere else.
    CHECK((cap.packets.front().payload()[1] & 0x80) != 0);
    CHECK((cap.packets.back().payload()[1] & 0x40) != 0);
    for (size_t i = 0; i < cap.packets.size(); i++) {
        const uint8_t ind = cap.packets[i].payload()[0];
        CHECK((ind & 0x1F) == 28);                                     // every fragment is FU-A
        CHECK((ind & 0xE0) == (nal[0] & 0xE0));                        // nri survives
        CHECK((cap.packets[i].payload()[1] & 0x1F) == (nal[0] & 0x1F)); // so does the type
    }

    // Reassembly: the original header byte, then every fragment's payload past its two FU bytes.
    std::vector<uint8_t> rebuilt{nal[0]};
    for (const auto& q : cap.packets)
        rebuilt.insert(rebuilt.end(), q.payload() + 2, q.payload() + q.payloadLen());
    CHECK(rebuilt.size() == nal.size());
    CHECK(std::memcmp(rebuilt.data(), nal.data(), nal.size()) == 0);
}

// The sequence advances by one per PACKET across frames, which is how a receiver detects loss. A per-frame counter would read every fragmented frame as a gap.
TEST_CASE("RtpH264 advances the sequence per packet across access units") {
    const auto au = annexB({{0x41, 0x01}});
    uint8_t scratch[mm::rtp::kMaxPacketBytes];
    Capture cap;
    mm::rtp::Packetiser p(3, 65534);                 // starts near the wrap

    p.writeAccessUnit(au.data(), au.size(), 0, scratch, sizeof(scratch), capture, &cap);
    p.writeAccessUnit(au.data(), au.size(), 3000, scratch, sizeof(scratch), capture, &cap);
    p.writeAccessUnit(au.data(), au.size(), 6000, scratch, sizeof(scratch), capture, &cap);
    REQUIRE(cap.packets.size() == 3);
    CHECK(cap.packets[0].seq() == 65534);
    CHECK(cap.packets[1].seq() == 65535);
    CHECK(cap.packets[2].seq() == 0);                // wraps, as a 16-bit counter does
}

// A refused sink abandons the whole frame: a torn access unit costs a decoder more than a missing one, which it simply skips.
TEST_CASE("RtpH264 abandons an access unit whose sink refuses") {
    std::vector<uint8_t> big(4000, 0x11);
    big[0] = 0x65;
    const auto au = annexB({big});
    uint8_t scratch[mm::rtp::kMaxPacketBytes];
    Capture cap;
    cap.refuseAfter = 1;                             // the second packet is refused
    mm::rtp::Packetiser p(4, 0);

    const size_t n = p.writeAccessUnit(au.data(), au.size(), 0, scratch, sizeof(scratch), capture, &cap);
    CHECK(n == 0);                                   // reported as written-nothing
}

// Annex B allows a 3-byte start code as well as a 4-byte one, and a hardware encoder emits both.
TEST_CASE("RtpH264 reads three-byte and four-byte start codes alike") {
    std::vector<uint8_t> au{0, 0, 1, 0x67, 0xAA, 0, 0, 0, 1, 0x65, 0xBB};
    uint8_t scratch[mm::rtp::kMaxPacketBytes];
    Capture cap;
    mm::rtp::Packetiser p(5, 0);

    const size_t n = p.writeAccessUnit(au.data(), au.size(), 0, scratch, sizeof(scratch), capture, &cap);
    REQUIRE(n == 2);
    CHECK(cap.packets[0].payload()[0] == 0x67);
    CHECK(cap.packets[1].payload()[0] == 0x65);
    CHECK(cap.packets[1].marker());
}
