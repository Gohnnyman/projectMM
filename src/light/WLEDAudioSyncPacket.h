#pragma once

#include "core/AudioFrame.h"

#include <cstdint>
#include <cstring>

namespace mm {

// WLED audio-sync wire format — the one place the packet layout lives (the
// ArtNetPacket.h / DdpPacket.h convention). AudioService builds it to broadcast
// its analyzed audio, and parses it to drive effects from a peer's audio; a unit
// test round-trips build↔parse against a golden byte vector so we can never drift
// from WLED. The contract is fixed by netmindz/WLED-sync (the header MoonModules'
// MoonLight receives with, D_WLEDAudio.h), so the bytes must be exact.
//
// v2 packet — 44 bytes, UDP port 11988, broadcast. Matches the "packed" struct in
// WLED-sync.h; we hand-serialise the exact offsets (like ArtNet/DDP) rather than
// rely on cross-compiler struct packing. The two "gap" runs are real wire bytes
// (WLED's struct has explicit gap1/gap2 padding), so they're part of the 44 and
// sent as zero. Floats are little-endian IEEE-754 (WLED memcpy's the struct on a
// little-endian MCU; every projectMM target is little-endian too, so a raw memcpy
// reproduces the exact bytes).
//
//   0-5   header "00002" (+ NUL)
//   6-7   gap1 (zero)
//   8-11  sampleRaw     float  — AudioFrame.level         (WLED volumeRaw)
//   12-15 sampleSmth    float  — AudioFrame.levelSmoothed (WLED volume/volumeSmth)
//   16    samplePeak    u8     — 1 = beat/peak this frame, else 0
//   17    reserved2     u8     : WLED: "for future extensions - not used yet". ZERO on the wire:
//                                 WLED sends 0 here and may claim the byte later, so a counter of
//                                 ours would be a private meaning in a field that is not ours.
//   18-33 fftResult[16] u8×16  : AudioFrame.bands[16], clamped to 254 (WLED's own send does
//                                 constrain(fftResult[i], 0, 254), so 255 never appears)
//   34-35 gap2 (zero)
//   36-39 FFT_Magnitude float  : AudioFrame.peakMag x kWledMagScale (see below)
//   40-43 FFT_MajorPeak float  — AudioFrame.peakHz

constexpr uint16_t WLED_SYNC_PORT = 11988;
constexpr size_t   WLED_SYNC_PACKET_SIZE = 44;
// 6 chars incl NUL: "00002". v1 packets use "00001" (83 bytes) — legacy, ignored.
constexpr char     WLED_SYNC_HEADER[6] = "00002";
constexpr size_t   WLED_SYNC_NUM_BANDS = 16;   // == AudioFrame bands + WLED NUM_GEQ_CHANNELS

// WLED's FFT_Magnitude is the raw magnitude of its dominant FFT bin, scaled so "the end result is
// linear and ~4096 max" (its own comment, where it divides the input samples by 16). Its effects
// then divide that by 16 and use it as a byte, which is why their thresholds read `< 48` squelch
// and `> 144` full brightness. AudioFrame::peakMag is byte-scaled 0..255 instead, conditioned by
// the same noise floor and gain as the 16 bands so one pair of knobs governs the whole spectrum.
//
// So the wire carries WLED's units and the AudioFrame keeps ours, converting at this boundary.
// x16 is not a fudge factor: it is exactly the divisor WLED's own effects apply, so a byte of ours
// lands on the same brightness a native WLED source would produce. Nothing internal changes, and
// the conversion costs one multiply per packet (~40/s), never per light.
inline constexpr float kWledMagScale = 16.0f;

// Little-endian IEEE-754 store/load. Every projectMM target (Xtensa/RISC-V ESP32,
// desktop x86/arm64) is little-endian, and so is WLED's source MCU — so the raw
// float bytes are the wire bytes. Kept as a helper so the intent is explicit and
// the round-trip test pins it.
inline void wledPutFloatLE(uint8_t* p, float v) { std::memcpy(p, &v, 4); }
inline float wledGetFloatLE(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

// Truncate a wire float into an AudioFrame uint16 field, bounded to [0, 65535].
// A foreign/garbage packet that passed the header check can carry NaN or an
// out-of-range magnitude; casting such a float straight to uint16_t is undefined,
// so clamp first. NaN fails both comparisons and falls through to 0.
inline uint16_t wledFloatToU16(float v) {
    if (!(v > 0.0f)) return 0;              // <= 0 or NaN
    if (v > 65535.0f) return 65535;
    return static_cast<uint16_t>(v);
}

// Build a v2 audio-sync packet from an AudioFrame into out (>= 44 bytes).
// `peak` is the one field not carried by AudioFrame (the caller owns the beat
// flag). Returns the packet size (44).
inline size_t buildWledAudioSync(uint8_t out[WLED_SYNC_PACKET_SIZE], const AudioFrame& f,
                                 bool peak) {
    std::memset(out, 0, WLED_SYNC_PACKET_SIZE);        // zeroes header pad + both gaps
    std::memcpy(out, WLED_SYNC_HEADER, 6);             // "00002\0"
    wledPutFloatLE(out + 8,  static_cast<float>(f.level));          // sampleRaw
    wledPutFloatLE(out + 12, static_cast<float>(f.levelSmoothed));  // sampleSmth
    out[16] = peak ? 1 : 0;                            // samplePeak
    // out[17] stays 0: reserved2 in WLED's struct, and WLED transmits zero there.
    // Bands clamp to 254, matching WLED's own constrain(fftResult[i], 0, 254): a receiver
    // written against WLED may treat 255 as a sentinel it never expects to see in real data.
    for (size_t i = 0; i < WLED_SYNC_NUM_BANDS; i++)
        out[18 + i] = f.bands[i] > 254 ? 254 : f.bands[i];           // fftResult[16]
    wledPutFloatLE(out + 36, static_cast<float>(f.peakMag) * kWledMagScale);   // FFT_Magnitude
    wledPutFloatLE(out + 40, static_cast<float>(f.peakHz));         // FFT_MajorPeak
    return WLED_SYNC_PACKET_SIZE;
}

// Parse + validate a v2 audio-sync packet into an AudioFrame (the inverse of
// build). Returns true only for a well-formed v2 datagram: exactly 44 bytes and
// the "00002" header. A v1 (83-byte "00001") packet, a short/foreign datagram, or
// a null buffer returns false so the caller ignores it (be strict on our own
// format, drop everything else — never crash). The float level/peak fields are
// truncated into the AudioFrame's small-integer fields, bounded to [0, 65535]
// (wledFloatToU16) so a foreign packet carrying NaN or an out-of-range value
// can't produce an undefined conversion.
inline bool parseWledAudioSync(const uint8_t* pkt, size_t len, AudioFrame& out) {
    if (!pkt || len != WLED_SYNC_PACKET_SIZE) return false;
    if (std::memcmp(pkt, WLED_SYNC_HEADER, 6) != 0) return false;
    out.level         = wledFloatToU16(wledGetFloatLE(pkt + 8));
    out.levelSmoothed = wledFloatToU16(wledGetFloatLE(pkt + 12));
    // pkt[16] samplePeak is a hint the AudioFrame doesn't carry; pkt[17] is WLED's reserved2.
    std::memcpy(out.bands, pkt + 18, WLED_SYNC_NUM_BANDS);
    // Back into our 0..255 units, and CLAMPED there: a WLED sender's magnitude can run well past
    // 255 after the divide, and letting it through would make a received frame drive effects
    // harder than a locally analyzed one ever could.
    const float mag = wledGetFloatLE(pkt + 36) / kWledMagScale;
    const uint16_t magU16 = wledFloatToU16(mag);
    out.peakMag = magU16 > 255 ? 255 : magU16;
    out.peakHz  = wledFloatToU16(wledGetFloatLE(pkt + 40));
    return true;
}

} // namespace mm
