// @module I80LedDriver
// @also Correction

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/ParallelSlots.h"

#include <cstring>

// The success spec for the LCD_CAM 3-slot encode, written RED before the
// encoder exists (the increment-1 methodology): a known wire row + lane mask
// → the exact slot-byte stream. Pins the transpose (one bus byte carries one
// bit of every lane), MSB-first bit order, and the lanes-active-mask rule
// that keeps short strands idle-LOW.

namespace {

// out holds channels*8 triplets of (slot0, slot1, slot2).
struct Slots {
    uint8_t bytes[4 * 8 * 3];   // up to 4 channels
    const uint8_t* triplet(int bit) const { return bytes + bit * 3; }
};

} // namespace

// One lane, one byte 0xA5: slot0 always the mask, slot1 follows the bits MSB-first, slot2 always zero.
TEST_CASE("LCD encoder: one lane, MSB-first, 3 slots per bit") {
    uint8_t wire[8 * 4] = {};
    wire[0] = 0xA5;   // lane 0, channel 0: 1010 0101
    Slots s{};
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x01), 1, s.bytes);

    const uint8_t expectBits[8] = {1, 0, 1, 0, 0, 1, 0, 1};
    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x01);                          // pulse start: active lanes HIGH
        CHECK(t[1] == (expectBits[bit] ? 0x01 : 0x00)); // data slot
        CHECK(t[2] == 0x00);                          // pulse tail: all LOW
    }
}

// Two lanes 0xFF/0x00 in one row: the data slot carries lane 0's bit only — the transpose itself.
TEST_CASE("LCD encoder: transpose across two lanes") {
    uint8_t wire[8 * 1] = {};   // stride = channels = 1
    wire[0 * 1 + 0] = 0xFF;   // lane 0: all ones
    wire[1 * 1 + 0] = 0x00;   // lane 1: all zeros
    Slots s{};
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x03), 1, s.bytes);

    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x03);   // both lanes pulse-start HIGH
        CHECK(t[1] == 0x01);   // only lane 0 carries a 1
        CHECK(t[2] == 0x00);
    }
}

// A lane excluded from the mask contributes to NEITHER slot 0 nor slot 1, even with garbage wire bytes — short strands idle LOW (no white flashes).
TEST_CASE("LCD encoder: inactive lanes stay LOW regardless of wire content") {
    uint8_t wire[8 * 4];
    std::memset(wire, 0xFF, sizeof(wire));   // garbage everywhere
    Slots s{};
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x01), 1, s.bytes);   // only lane 0 active

    for (int bit = 0; bit < 8; bit++) {
        const uint8_t* t = s.triplet(bit);
        CHECK(t[0] == 0x01);   // lane 1..7 absent from the pulse start
        CHECK(t[1] == 0x01);   // and from the data slot
        CHECK(t[2] == 0x00);
    }
}

// Mask 0 (a row past every lane's strand) is a fully idle row.
TEST_CASE("LCD encoder: empty mask emits all-zero slots") {
    uint8_t wire[8 * 4];
    std::memset(wire, 0xFF, sizeof(wire));
    Slots s{};
    std::memset(s.bytes, 0xEE, sizeof(s.bytes));
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x00), 1, s.bytes);
    for (int i = 0; i < 8 * 3; i++) CHECK(s.bytes[i] == 0x00);
}

// Channel order comes from Correction (logical red → GRB wire {0,255,0}); the encoder is order-agnostic.
TEST_CASE("LCD encoder: GRB ordering via Correction") {
    mm::Correction corr;
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    const uint8_t rgb[3] = {255, 0, 0};   // logical red
    uint8_t wire[8 * 4] = {};
    corr.apply(rgb, wire);                 // lane 0 wire = {0, 255, 0}

    Slots s{};
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x01), 3, s.bytes);
    for (int bit = 0; bit < 8; bit++) {
        CHECK(s.triplet(bit)[1] == 0x00);        // G byte: all zero data
        CHECK(s.triplet(8 + bit)[1] == 0x01);    // R byte: all ones data
        CHECK(s.triplet(16 + bit)[1] == 0x00);   // B byte: all zero data
    }
}

// RGBW rows emit 4 channels × 8 bits × 3 slots = 96 bytes.
TEST_CASE("LCD encoder: RGBW row is 96 slot bytes") {
    mm::Correction corr;
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRBW);
    const uint8_t rgb[3] = {10, 10, 10};
    uint8_t wire[8 * 4] = {};
    corr.apply(rgb, wire);

    uint8_t out[4 * 8 * 3];
    std::memset(out, 0xEE, sizeof(out));
    mm::encodeWs2812ParallelSlots(wire, static_cast<uint8_t>(0x01), 4, out);
    // The last triplet was written (its tail slot is 0, not the 0xEE poison).
    CHECK(out[4 * 8 * 3 - 1] == 0x00);
}

// The branch-free SWAR transpose must equal the naive per-bit-per-lane gather
// it replaced, for EVERY lane pattern and mask — the whole point is a
// behavior-identical speedup. Pin it directly (not just via a few golden rows)
// so a future edit to the delta-swap constants can't silently corrupt a plane.
TEST_CASE("LCD encoder: SWAR transpose equals the naive gather for all patterns") {
    auto naiveData = [](const uint8_t* lane8, uint8_t mask, int bit) -> uint8_t {
        uint8_t data = 0;
        for (uint8_t lane = 0; lane < 8; lane++)
            if (mask & (1u << lane))
                data |= static_cast<uint8_t>(((lane8[lane] >> bit) & 1u) << lane);
        return data;
    };
    uint8_t lanes[8];
    for (int trial = 0; trial < 4096; trial++) {
        uint32_t s = static_cast<uint32_t>(trial) * 2654435761u + 1u;
        for (int i = 0; i < 8; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; lanes[i] = s & 0xFF; }
        const uint8_t mask = static_cast<uint8_t>((trial * 40503u) & 0xFF);
        uint8_t masked[8];
        for (int i = 0; i < 8; i++) masked[i] = (mask & (1u << i)) ? lanes[i] : 0;
        uint8_t plane[8];
        mm::transposeLanes8x8(masked, plane);
        for (int bit = 0; bit < 8; bit++) CHECK(plane[bit] == naiveData(lanes, mask, bit));
    }
}

// The 16-lane transpose (transposeLanes16x8, for the 16-bit bus) must equal the
// naive 16-lane gather for EVERY pattern and mask — the low byte of each uint16
// plane is lanes 0..7, the high byte lanes 8..15. Cycles mask shapes including
// high-lane-only (lanes 8..15 set, 0..7 clear), the case the byte-split hinges on.
TEST_CASE("LCD encoder: SWAR 16-lane transpose equals the naive gather") {
    auto naive16 = [](const uint8_t* lane16, uint16_t mask, int bit) -> uint16_t {
        uint16_t d = 0;
        for (uint8_t lane = 0; lane < 16; lane++)
            if (mask & (1u << lane))
                d |= static_cast<uint16_t>(((lane16[lane] >> bit) & 1u) << lane);
        return d;
    };
    uint8_t lanes[16];
    for (int trial = 0; trial < 4096; trial++) {
        uint32_t s = static_cast<uint32_t>(trial) * 2246822519u + 7u;
        for (int i = 0; i < 16; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; lanes[i] = s & 0xFF; }
        uint16_t mask;
        if (trial % 3 == 0)      mask = static_cast<uint16_t>(trial * 40503u);
        else if (trial % 3 == 1) mask = static_cast<uint16_t>((trial * 40503u) & 0x00FF);
        else                     mask = static_cast<uint16_t>((trial * 40503u) & 0xFF00);
        uint8_t masked[16];
        for (int i = 0; i < 16; i++) masked[i] = (mask & (1u << i)) ? lanes[i] : 0;
        uint16_t plane[8];
        mm::transposeLanes16x8(masked, plane);
        for (int bit = 0; bit < 8; bit++) CHECK(plane[bit] == naive16(lanes, mask, bit));
    }
}

// 16-lane golden encoder cases: the uint16 slot carries 16 data lines. Prove a
// high lane (15) lands in the plane's high byte, and a boundary pair (7 + 8).
// The wire lane stride is `channels` (here 1), so lane L's byte 0 is wire[L * channels].
TEST_CASE("LCD encoder 16-lane: high lane and byte-boundary transpose") {
    uint16_t out[3 * 8];   // 1 channel × 8 bits × 3 slots, uint16 slots
    {   // lane 15 all-ones → data slot has bit 15 set on every bit (stride = channels = 1)
        uint8_t wire[16 * 1] = {};
        wire[15 * 1] = 0xFF;
        mm::encodeWs2812ParallelSlots<uint16_t>(wire, static_cast<uint16_t>(1u << 15), 1, out);
        for (int bit = 0; bit < 8; bit++) {
            CHECK(out[bit * 3 + 0] == (1u << 15));   // slot0: active mask (lane 15)
            CHECK(out[bit * 3 + 1] == (1u << 15));   // slot1: lane 15's bit
            CHECK(out[bit * 3 + 2] == 0);            // slot2: tail
        }
    }
    {   // lane 7 = 0xFF, lane 8 = 0xFF → data slot has bits 7 AND 8 (across the byte split)
        uint8_t wire[16 * 1] = {};
        wire[7 * 1] = 0xFF; wire[8 * 1] = 0xFF;
        const uint16_t mask = static_cast<uint16_t>((1u << 7) | (1u << 8));
        mm::encodeWs2812ParallelSlots<uint16_t>(wire, mask, 1, out);
        for (int bit = 0; bit < 8; bit++)
            CHECK(out[bit * 3 + 1] == mask);         // both lanes carry a 1 on every bit
    }
}

// RGBW 16-lane row: 4 channels × 8 bits × 3 slots = 96 uint16 slots, all written.
TEST_CASE("LCD encoder 16-lane: RGBW row is 96 uint16 slots, all written") {
    uint8_t wire[16 * 4] = {};
    wire[0] = 10; wire[1] = 10; wire[2] = 10; wire[3] = 10;   // lane 0 RGBW
    uint16_t out[4 * 8 * 3];
    for (auto& v : out) v = 0xEEEE;   // poison
    mm::encodeWs2812ParallelSlots<uint16_t>(wire, static_cast<uint16_t>(0x0001), 4, out);
    CHECK(out[4 * 8 * 3 - 1] == 0);   // last tail slot written (not the poison)
}
