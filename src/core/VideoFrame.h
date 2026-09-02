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
    uint32_t seq = 0; // bumped per new frame; compare for INEQUALITY, never ordering.
};

// The "no source" frame consumers fall back to.
inline constexpr VideoFrame kNoVideoFrame{};

} // namespace mm
