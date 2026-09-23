#pragma once

#include <cstdint>

namespace mm {

// One decoded video frame, produced by VideoService and read by video-reactive effects. Same
// plain-struct contract as AudioFrame, except a frame is hundreds of kilobytes, so this borrows a
// pointer to the producer's buffer rather than carrying the pixels.
//
// `rgb` is valid only until VideoService's next tick: hold it for one effect tick, never across
// frames. Before any frame exists it is null, which every consumer must tolerate.
struct VideoFrame {
    const uint8_t* rgb = nullptr; // width*height*3, row-major, top-left origin, no padding
    uint16_t width = 0;
    uint16_t height = 0;
    // Bumped per PUBLISHED frame; compare for INEQUALITY, never ordering. A still PPM bumps it
    // every tick, the way a camera aimed at a still object sends one every period.
    uint32_t seq = 0;
    // 256-entry curve from the source's encoding to LINEAR light, 0..kLinearMax; SDR is sRGB, a
    // curve like any other, so a published frame always carries one. Same one-tick lifetime as
    // `rgb`. Read pixels through channel(): a consumer averaging the encoded bytes averages a
    // quantity that is not proportional to light.
    const uint16_t* tone = nullptr;

    // 12 bits, not 8: linear has no headroom at the dark end, which is what encodings exist for.
    // Small enough that a zone of ~1M pixels still sums inside a uint32.
    static constexpr uint16_t kLinearMax = 4095;

    /// One channel of the pixel at `px` as linear light: one lookup on a read the caller already
    /// makes. The encoded byte as is when no curve is published (a test frame).
    uint16_t channel(const uint8_t* px, int c) const { return tone ? tone[px[c]] : px[c]; }
};

// The "no source" frame consumers fall back to.
inline constexpr VideoFrame kNoVideoFrame{};

} // namespace mm
