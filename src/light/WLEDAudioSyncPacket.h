#pragma once

#include "core/AudioFrame.h"

#include <cstdint>
#include <cstring>

namespace mm {

// WLED audio-sync wire format — the one place the packet layout lives (the
// ArtNetPacket.h / DdpPacket.h convention). AudioService builds it to broadcast
// its analysed audio, and parses it to drive effects from a peer's audio; a unit
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
//   17    frameCounter  u8     — increments per send, for dup/reorder detection
//   18-33 fftResult[16] u8×16  — AudioFrame.bands[16]     (the 16 GEQ channels)
//   34-35 gap2 (zero)
//   36-39 FFT_Magnitude float  — AudioFrame.peakMag
//   40-43 FFT_MajorPeak float  — AudioFrame.peakHz

constexpr uint16_t WLED_SYNC_PORT = 11988;
constexpr size_t   WLED_SYNC_PACKET_SIZE = 44;
// 6 chars incl NUL: "00002". v1 packets use "00001" (83 bytes) — legacy, ignored.
constexpr char     WLED_SYNC_HEADER[6] = "00002";
constexpr size_t   WLED_SYNC_NUM_BANDS = 16;   // == AudioFrame bands + WLED NUM_GEQ_CHANNELS

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
// `frameCounter` and `peak` are the two fields not carried by AudioFrame (the
// caller owns the send counter and the beat flag). Returns the packet size (44).
inline size_t buildWledAudioSync(uint8_t out[WLED_SYNC_PACKET_SIZE], const AudioFrame& f,
                                 uint8_t frameCounter, bool peak) {
    std::memset(out, 0, WLED_SYNC_PACKET_SIZE);        // zeroes header pad + both gaps
    std::memcpy(out, WLED_SYNC_HEADER, 6);             // "00002\0"
    wledPutFloatLE(out + 8,  static_cast<float>(f.level));          // sampleRaw
    wledPutFloatLE(out + 12, static_cast<float>(f.levelSmoothed));  // sampleSmth
    out[16] = peak ? 1 : 0;                            // samplePeak
    out[17] = frameCounter;                            // frameCounter
    std::memcpy(out + 18, f.bands, WLED_SYNC_NUM_BANDS);            // fftResult[16]
    wledPutFloatLE(out + 36, static_cast<float>(f.peakMag));        // FFT_Magnitude
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
    // pkt[16] samplePeak and pkt[17] frameCounter are hints the AudioFrame doesn't carry.
    std::memcpy(out.bands, pkt + 18, WLED_SYNC_NUM_BANDS);
    out.peakMag = wledFloatToU16(wledGetFloatLE(pkt + 36));
    out.peakHz  = wledFloatToU16(wledGetFloatLE(pkt + 40));
    return true;
}

} // namespace mm
