#pragma once

#include <cstddef>   // size_t (the shift-register encoder's wire indexing)
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

// ---------------------------------------------------------------------------
// SHIFT-REGISTER encode — the same WS2812 wire contract, fanned out through
// 74HCT595 expanders so each physical data pin drives `outputsPerPin` strands.
// Named for the hardware (a shift register is what it is); the lineage this
// studied calls it a "virtual" driver, but nothing here is virtual — the '595s,
// the latch line and the level shifter are all physically on the board.
// Prior art: hpwit's I2SClocklessVirtualLedDriver — studied, not copied.
//
// A '595 is SERIAL-IN, parallel-out: presenting 8 bits takes 8 clock cycles. So
// each of the 3 WS2812 slots above becomes `outputsPerPin` SHIFT CYCLES here —
// the bus word's bit P carries physical pin P's bit for one expanded output, and
// the peripheral's own pixel clock (WR, already the i80 `clockPin`) is the shift
// clock. That is the whole cost model: the fan-out multiplies the slot COUNT
// (memory + wire time) but NOT the pin count, so extra lanes are free while the
// ×8 itself is not.
//
// LATCH is a DATA lane (one bit of every bus word), not a peripheral pin: the
// '595s present their shifted byte on its rising edge. It is pulsed on the LAST
// shift cycle of each slot, once all `outputsPerPin` bits are in. `latchBit` is
// its bus-bit index; the caller keeps it out of the data pins.
//
// Lane numbering: lane V lives on physical pin (V / outputsPerPin) at shift
// position (V % outputsPerPin). Shift cycle c therefore gathers, for each physical
// pin P, lane (P * outputsPerPin + c) — and that gather is STILL an 8-lane
// bit-plane transpose, so the SWAR core above is reused unchanged.
//
// A '595 shifts MSB-of-the-register-first: the bit clocked in FIRST ends up on the
// LAST output (QH). So cycle c must carry the byte for shift position
// (outputsPerPin - 1 - c) to land lane V on the output the wiring calls V.

/// Outputs per physical data pin when a 74HCT595 expander is fitted (one '595 = 8).
/// The fan-out is a property of the board, not a free parameter.
///
/// **74HCT, not 74HC — the T is load-bearing.** (Plain-'HC parts are known to misbehave here; that
/// is the reason the board specifies HCT.) Both are 8-bit shift registers; they differ in INPUT
/// threshold, and the ESP32 drives 3.3 V logic into a part powered at 5 V:
///   - **74HC** at 5 V: V_IH(min) = 0.7 × Vcc = **3.5 V** → a 3.3 V HIGH is BELOW threshold and is
///     not guaranteed to read as a 1. It often *appears* to work (a given chip may trip nearer
///     2.5 V at room temperature), then fails with temperature, supply, or a new batch — the
///     symptom is flaky/garbled strands, not dead ones, which is what makes it so hard to chase.
///   - **74HCT** at 5 V: TTL-compatible inputs, V_IH(min) = **2.0 V** → 3.3 V is comfortably a HIGH,
///     while the outputs still swing a full 5 V, which is what the WS2812 data line wants.
///
/// The target board is hpwit's expander. Its BOM for 48 strands, read off the fitted parts:
///   - **6 × 74HCT595N** (NXP) — the shift registers: the actual 1→8 fan-out (6 × 8 = 48).
///   - **1 × SN74HCT245N** (TI) — an OCTAL buffer/level-shifter (it stores nothing): 3.3 V→5 V,
///     and it supplies the drive current.
///
/// **One '245 is exactly enough for THIS board, and that fact is what caps it at 6 data pins.** The
/// signals needing the shift are `dataPins + CLOCK + LATCH`, and a '245 is 8 channels wide — so
/// 6 + 2 = 8 fills it exactly. It is *not* a general property of the design: hpwit's 15-pin variant
/// needs 15 + 2 = 17 signals = **three** '245s. A board with one '245 can never be a 15-pin board.
///
/// **Timing headroom is thin, and the buffer is the reason — know this before blaming the code.**
/// The fitted '245 is plain **HCT** (t_pd ≈ 10–18 ns at 5 V), not the ~5 ns **A**HCT part. Shift mode
/// clocks the bus at 26.67 MHz — a 37.5 ns period — so the buffer alone can eat a third of the bit
/// period in propagation delay. hpwit runs a comparable rate (19.2 MHz) on this hardware, so it *does*
/// work; but there is little margin. **If a board is clean at the direct rate and flaky only through
/// the expander, suspect the buffer's timing before the firmware** — an AHCT245 is the drop-in that
/// buys the margin back.
///
/// Do not "simplify" any of these to a non-T part — see the threshold arithmetic above.
///
/// **8 outputs — one 74HCT595 per data pin. Cascading two ('595 -> '595 via Q7') would give 16, but
/// ×16 is NOT offered, and the reason is the clock, not the code.
///
/// The bus rate is set by the shift DEPTH: each WS2812 slot must still last its 300 ns, filled by
/// the fan-out's shift cycles, so doubling the depth doubles the required pclk. An in-spec slot needs
/// 290-380 ns (T0H ≤ 380, T1H = 2 × slot ≥ 580), which puts ×16 at **42.1-55.2 MHz** — and the
/// LCD_CAM bus resolution (80 MHz) has **no exact divide in that band** (its divides are 80, 40, 20,
/// 16, 10 MHz). esp_lcd silently rounds an inexact request DOWN to an integer prescale, so asking for
/// ~53 MHz yields **80 MHz**: a 200 ns slot, T1H 400 ns, far below the 580 ns floor. ×16 cannot emit a
/// valid WS2812 waveform here at all.
///
/// (×8's band is 21.1-27.6 MHz, and 80/3 = 26.67 MHz sits inside it — the shipped rate is provably the
/// only exact divide that works. A P4's APLL could synthesise an arbitrary rate and would be the only
/// route to ×16; that is a separate driver-level change, not a config flag.)
///
/// **Grow on PINS, not on cascade depth** anyway: pin count is parallel, so it does not touch the
/// clock and it does not grow the DMA frame — hpwit's 120-strand headline is 15 pins × 8, not a
/// deeper chain.
inline constexpr uint8_t kShiftOutputs = 8;   // one 74HCT595 per data pin

/// Close a shift-register frame: one latch-only bus word, written at the START of the latch pad.
///
/// **Without this the frame never resets, content-dependently.** The '595 pipeline is one slot deep —
/// the byte clocked during slot N is presented during slot N+1 — so the LAST slot of the last light
/// needs a latch edge *after* it. The pad is all zeros and carries no latch bit, so the register
/// would keep presenting the final DATA byte for the pad's whole ≥300 µs: a strand whose last wire
/// byte is EVEN idles LOW and resets correctly, while one whose last byte is ODD idles **HIGH** and
/// never sees the WS2812 reset at all — the next frame's bits then append to an unlatched stream and
/// that strand garbles. (Direct mode has no such hazard: with no register in the path, a zeroed pad
/// IS a LOW line.)
///
/// One word (data lanes LOW + the latch bit) latches the trailing zeros through, and the rest of the
/// zeroed pad then holds every strand LOW. Call once, immediately after the last row.
template <class Slot>
inline void encodeWs2812ShiftLatchPad(uint8_t latchBit, Slot* out) {
    *out = static_cast<Slot>(Slot(1) << latchBit);
}

/// Encode one ROW through the shift-register expander.
///   wire:        lanes × `channels` corrected wire bytes, lane-major — as the direct
///                encoder, but indexed by EXPANDED lane.
///   activeMask:  bit V set = lane V drives this row. Up to kMaxLanes lanes (more than
///                the bus is wide), so it is a uint64_t, not a Slot.
///   physPins:    physical data pins in use (lanes ≤ physPins × outPerPin).
///   latchBit:    bus-bit index of the LATCH line (never a data pin).
///   outPerPin:   the fan-out — kShiftOutputs (8, one '595). A runtime parameter, not a constant,
///                so the cost model stays explicit; see kShiftOutputs for why 16 is not offered.
///   channels:    wire bytes per light (also the lane stride).
///   out:         channels * 8 * 3 * outPerPin SLOTS, fully written.
template <class Slot>
inline void encodeWs2812ShiftSlots(const uint8_t* wire, uint64_t activeMask,
                                   uint8_t physPins, uint8_t latchBit, uint8_t outPerPin,
                                   uint8_t channels, Slot* out) {
    constexpr uint8_t kLanes = sizeof(Slot) * 8;   // bus width: 8 or 16
    if (outPerPin == 0 || outPerPin > kShiftOutputs) return;
    const Slot latch = static_cast<Slot>(Slot(1) << latchBit);
    for (uint8_t ch = 0; ch < channels; ch++) {
        // Bit-planes per shift cycle: plane[c][bit] bit P = physical pin P's byte-bit `bit` for
        // the lane it drives on cycle c. Built by reusing the SWAR transpose once per cycle over
        Slot plane[kShiftOutputs][8];
        // Active pins PER SHIFT CYCLE, not per pin. Cycle c clocks in the bit for shift position
        // `pos` of every pin, so what belongs there is "is the strand at (pin, pos) active?" — a
        // per-STRAND question. Aggregating one mask across all cycles ("pin P has some live lane")
        // would drive the pulse-start HIGH on a cycle whose strand is inactive: two strands sharing
        // a '595 (one long, one short) would make the short one flash white on every WS2812 pulse.
        Slot activePins[kShiftOutputs] = {};
        for (uint8_t c = 0; c < outPerPin; c++) {
            // '595 shifts MSB-first: the first bit clocked in lands on the last output.
            const uint8_t pos = static_cast<uint8_t>(outPerPin - 1 - c);
            uint8_t lanes[kLanes] = {};
            for (uint8_t p = 0; p < physPins && p < kLanes; p++) {
                const uint8_t v = static_cast<uint8_t>(p * outPerPin + pos);
                if (!(activeMask & (uint64_t(1) << v))) continue;   // inactive: idle LOW
                lanes[p] = wire[static_cast<size_t>(v) * channels + ch];
                activePins[c] |= static_cast<Slot>(Slot(1) << p);
            }
            if constexpr (sizeof(Slot) == 1) transposeLanes8x8(lanes, plane[c]);
            else                             transposeLanes16x8(lanes, plane[c]);
        }
        for (int bit = 7; bit >= 0; bit--) {   // MSB-first per byte, as the wire contract
            // Each WS2812 slot is shifted out over outPerPin bus words. The i80 WR (pixel clock) IS
            // the '595 shift clock, so EVERY bus word produces one SRCLK edge — including the word
            // that carries the latch bit.
            //
            // **The latch therefore goes on the FIRST word of a slot, not the last.** RCLK is
            // rising-edge triggered: it must fire when the slot's 8 bits are already all in, which is
            // one word AFTER the last shift word — i.e. the first word of the NEXT slot. Putting it
            // on the last word (the obvious-looking choice, and what this encoder did first) asserts
            // RCLK *while the 8th bit is still being clocked in*, so the '595 presents a byte shifted
            // one position short. Bench symptom (board B, 2026-07-14): only the first LED or two of
            // every strand lit — a regular one-pixel-per-panel grid. The data words the latch rides
            // are harmless: they are only ENTERING the shift register, not being latched.
            //
            // Verified against hpwit's driver, which does the same on its S3/LCD_CAM path
            // (`putdefaultlatch`: `buff[i * (NUM_VIRT_PINS + 1)] = mask1`, i.e. word 0 of each slot).
            // **THE ONE-SLOT PIPELINE.** A '595 only updates its OUTPUTS on the latch, so during a
            // slot's shift cycles the strand sees the byte latched at the START of that slot — i.e.
            // the byte shifted in during the PREVIOUS slot. The hardware is a one-slot delay line.
            //
            // So the encoder must shift each value in ONE SLOT EARLY. Writing "the value I want the
            // strand to see" into the slot where I want it seen (the intuitive layout, and what this
            // shipped first) makes the strand receive [tail][start][data] instead of
            // [start][data][tail]: the first LED survives (empty pipeline) and everything after it is
            // noise — the exact bench symptom on board B (2026-07-14).
            //
            // Hence the rotation below: slot 0 carries what the strand must SEE in slot 1, and so on.
            // hpwit does the same by biasing his DMA write pointer a whole slot (`buff += OFFSET`).
            //
            // Wrap-around is safe: the LAST slot of a bit carries the FIRST slot of the next bit (the
            // pulse start), and every WS2812 bit begins with that same all-HIGH pulse — so the value
            // is identical whichever bit it belongs to. The frame's leading slot (shifted in before
            // any latch) and the zeroed latch pad at the end absorb the ends of the pipeline.
            // Each slot clocks in the byte the strand will SEE one slot later (the pipeline above),
            // and the '595 needs a full byte per slot — 8 words, one bit each:
            //   - to PRESENT all-HIGH (pulse start), clock in 0xFF for the ACTIVE strands: word c
            //     sets the data bit of each pin whose strand at shift position c is active
            //     (`activePins[c]`), so an exhausted strand keeps clocking in 0 and stays dark.
            //   - to PRESENT the data bit, clock in the transposed plane: word c carries, for each
            //     pin, the bit of the strand at shift position c. That is `plane[c][bit]`.
            //   - to PRESENT all-LOW (tail), clock in 0x00: eight zero words.
            for (uint8_t c = 0; c < outPerPin; c++) {
                const Slot first = (c == 0) ? latch : Slot(0);   // RCLK on word 0 of each slot
                // clocked in slot N  ->  seen by the strand in slot N+1
                out[c]                 = static_cast<Slot>(activePins[c] | first);   // -> seen: pulse start (HIGH)
                out[outPerPin + c]     = static_cast<Slot>(plane[c][bit] | first);   // -> seen: the data bit
                out[2 * outPerPin + c] = first;                                      // -> seen: pulse tail (LOW)
            }
            out += 3 * outPerPin;
        }
    }
}

} // namespace mm
