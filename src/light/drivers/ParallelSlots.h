#pragma once

#include <cstdint>

namespace mm {

// WS2812 encode for parallel WS2812 buses — the contract between a parallel
// driver (domain) and a parallel peripheral, named for the wire unit it builds
// (one pixel-clock SLOT = one byte on the 8-bit bus), the RmtSymbol.h sibling.
// Used by BOTH the LCD_CAM i80 driver (ESP32-S3, I80LedDriver) and the Parlio
// driver (ESP32-P4, ParlioLedDriver) — a Parlio bus byte and an i80 bus byte
// are identical (one word per slot, bit L = data line L), so one encoder
// serves both. Pure data transform, no platform include — the host CI encoder
// test (unit_ParallelSlots.cpp) pins it with no ESP32.
//
// Technique (hpwit / Adafruit "ESP32uesday" / FastLED S3 lineage — studied,
// not copied): every WS2812 data bit becomes THREE bus slots clocked at
// 2.67 MHz (slot = 375 ns, bit = 1.125 µs):
//
//   slot 0: activeMask        — every active lane HIGH (the pulse start)
//   slot 1: data bits & mask  — lane L's current bit at bus bit L
//   slot 2: 0x00              — every lane LOW (the pulse tail)
//
// so a "1" bit is HIGH for 2 slots (750 ns ≈ t1h 700 ns) and a "0" bit for
// 1 slot (375 ns ≈ t0h 350 ns). The LedDriverConfig nanosecond fields are
// APPROXIMATED by the slot clock — timing is fixed by the pclk (chosen in
// platform_esp32_i80.cpp; 375 ns keeps T0H inside even the newest WS2812B
// revisions' ~380 ns max — longer "0" pulses wash strips out white on a
// direct 3.3 V data line).
//
// Lanes-active-mask rule: a lane whose strand is shorter than the longest one
// must appear in NEITHER slot 0 nor slot 1 once its lights are exhausted —
// excluded lanes idle LOW for the rest of the frame instead of flashing white.
// The caller expresses that by clearing the lane's bit in `activeMask`.
//
// Bus bit L = the L-th entry of the driver's `pins` list (D0 = first pin).
// Bits go MSB-first per byte; channel order (GRB, …) is already applied by
// Correction before the encode, so the encoder is order-agnostic (same
// contract as encodeWs2812Symbols).
//
// The data slot is an 8×8 BIT-MATRIX TRANSPOSE: 8 lane bytes (rows) → 8 bus
// bytes (one per data bit, the columns), byte b bit L = lane L's bit b. This
// is the measured render-loop hot spot (docs/backlog/multicore-analysis-*: the
// transpose is ~85% of the driver frame at 16K lights), so it uses the
// branch-free SWAR transpose (Warren, *Hacker's Delight* §7-3 "delta swap";
// the same 3-step 64-bit trick FastLED's transpose8x1 uses) instead of a
// per-bit-per-lane gather loop — same result, no table, ~an order fewer ops.
// Studied, not copied; pinned bit-perfect by unit_ParallelSlots.cpp + the
// on-device loopback self-test.

// Transpose 8 lane bytes into 8 bit-plane bytes: out[b] bit L = in[L] bit b.
// Inactive lanes must be passed as 0 (the caller masks them) so they contribute
// no set bit to any plane. Three delta-swaps on the packed 64-bit matrix.
inline void transposeLanes8x8(const uint8_t* in, uint8_t* out) {
    uint64_t x = 0;
    for (int r = 0; r < 8; r++) x |= static_cast<uint64_t>(in[r]) << (8 * r);
    uint64_t t;
    t = (x ^ (x >> 7))  & 0x00AA00AA00AA00AAULL; x = x ^ t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL; x = x ^ t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL; x = x ^ t ^ (t << 28);
    for (int c = 0; c < 8; c++) out[c] = static_cast<uint8_t>(x >> (8 * c));
}

// Transpose 16 lane bytes into 8 bit-plane WORDS: out[b] bit L = in[L] bit b, for
// the 16-lane (16-bit bus) drivers. A uint16 plane splits at the byte boundary —
// its low byte is lanes 0..7, its high byte lanes 8..15 — and those two halves are
// INDEPENDENT 8-lane transposes, so this reuses the (already bit-perfect pinned)
// 8×8 SWAR core twice rather than a bespoke 128-bit trick: same textbook construct,
// no new magic constants. (If profiling ever shows the two-pass combine is the
// ceiling, a fused 128-bit SWAR is a drop-in behind this signature + the same test.)
// Inactive lanes must be passed as 0 by the caller, as with transposeLanes8x8.
inline void transposeLanes16x8(const uint8_t* in, uint16_t* out) {
    uint8_t lo[8], hi[8];
    transposeLanes8x8(in,     lo);   // lanes 0..7  → low byte of each plane
    transposeLanes8x8(in + 8, hi);   // lanes 8..15 → high byte of each plane
    for (int b = 0; b < 8; b++)
        out[b] = static_cast<uint16_t>(lo[b]) | static_cast<uint16_t>(hi[b] << 8);
}

// Encode one ROW (the same light index across all lanes) into 3-slot bus words.
//   Slot:       uint8_t for an 8-lane (8-bit) bus, uint16_t for a 16-lane (16-bit)
//               bus — one bus word per slot, bit L = data line L, so the word width
//               IS the lane count. Deduced from the call, so the 8-bit call sites
//               (uint8_t mask + uint8_t* out) are source-unchanged.
//   wire:       kMaxLanes × `channels` corrected wire bytes, lane-major
//               (wire[lane * channels + channel]); only lanes set in activeMask are
//               read — inactive lanes may hold garbage. The lane stride IS `channels`
//               (not a fixed 4), so a light of any channel count (RGB / RGBW / RGBCCT /
//               an N-channel fixture) is laid out without overrun — the caller sizes
//               wire to kMaxLanes × channels.
//   activeMask: bit L set = lane L drives this row (8 or 16 bits wide = Slot).
//   channels:   wire bytes per light (3 RGB / 4 RGBW / 5 RGBCCT / …), also the lane stride.
//   out:        channels * 8 * 3 SLOTS (Slot elements), fully written.
template <class Slot>
inline void encodeWs2812ParallelSlots(const uint8_t* wire, Slot activeMask,
                                 uint8_t channels, Slot* out) {
    constexpr uint8_t kLanes = sizeof(Slot) * 8;   // 8 or 16
    for (uint8_t ch = 0; ch < channels; ch++) {
        // Gather this channel's byte from each lane, zeroing inactive lanes so
        // they contribute no set bit to any plane (the idle-LOW rule), then
        // transpose all 8 bits × kLanes lanes in one pass. The lane stride is
        // `channels` (the wire is laid out kLanes × channels), so any channel count fits.
        uint8_t lanes[kLanes];
        for (uint8_t lane = 0; lane < kLanes; lane++)
            lanes[lane] = (activeMask & (Slot(1) << lane)) ? wire[lane * channels + ch] : 0;
        Slot plane[8];
        if constexpr (sizeof(Slot) == 1) transposeLanes8x8(lanes, plane);
        else                             transposeLanes16x8(lanes, plane);
        for (int bit = 7; bit >= 0; bit--) {   // MSB-first per byte
            *out++ = activeMask;    // slot 0: pulse start (active lanes HIGH)
            *out++ = plane[bit];    // slot 1: bit `bit` of every active lane
            *out++ = 0;             // slot 2: pulse tail (all LOW)
        }
    }
}

} // namespace mm
