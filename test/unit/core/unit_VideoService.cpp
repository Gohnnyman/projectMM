// @module VideoService

#include "doctest.h"
#include "core/VideoService.h"

#include <cstdint>
#include <cstring>

// Pins the PPM header grammar VideoService's file source accepts. The parser is the part with real
// edge cases: comments, whitespace runs, a 16-bit maxval, a truncated header, and it decides
// where pixel data starts, so getting the offset wrong shows as a picture shifted by a few bytes
// rather than as a clean failure. Driven directly (it is a pure static) so these run without a
// filesystem, and so a malformed file is testable without writing one.

using mm::VideoService;

namespace {
// Parse helper: returns the pixel offset, or -1, and reports the dimensions it read.
int parse(const char* text, uint16_t& w, uint16_t& h) {
    return VideoService::parsePpmHeader(text, static_cast<int>(std::strlen(text)), w, h);
}
} // namespace

// The canonical form ffmpeg and ImageMagick emit: magic, dimensions, maxval, one newline, pixels.
TEST_CASE("VideoService PPM: a canonical P6 header yields the dimensions and the pixel offset") {
    uint16_t w = 0, h = 0;
    const char* hdr = "P6\n64 36\n255\n";
    CHECK(parse(hdr, w, h) == 13); // pixels begin straight after the final newline
    CHECK(w == 64);
    CHECK(h == 36);
}

// Netpbm allows any run of whitespace between tokens and `#` comments to end of line: both appear
// in real files (GIMP writes a comment), so both must be skipped without shifting the offset.
TEST_CASE("VideoService PPM: comments and whitespace runs are skipped, not counted as pixels") {
    uint16_t w = 0, h = 0;
    const char* hdr = "P6\n# CREATOR: GIMP\n  16   9  \n255\n";
    const int off = parse(hdr, w, h);
    CHECK(off == static_cast<int>(std::strlen(hdr)));
    CHECK(w == 16);
    CHECK(h == 9);
}

// Exactly ONE whitespace byte separates the header from the binary block; any further byte is
// already a pixel. Consuming two would tint the whole image by shifting every channel one place.
TEST_CASE("VideoService PPM: only one separator byte is consumed before the pixels") {
    uint16_t w = 0, h = 0;
    // A leading pixel byte that happens to be whitespace-valued (0x20) must survive as data.
    const char hdr[] = {'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n', ' ', 'X'};
    const int off = VideoService::parsePpmHeader(hdr, static_cast<int>(sizeof(hdr)), w, h);
    CHECK(off == 11); // after the newline: NOT after the following 0x20
    CHECK(w == 2);
    CHECK(h == 2);
}

// P3 is the ASCII variant: same dimensions, completely different body (decimal text, not bytes).
// Accepting it would read numerals as pixel values and render noise.
TEST_CASE("VideoService PPM: the ASCII variant P3 is rejected, not read as binary") {
    uint16_t w = 0, h = 0;
    CHECK(parse("P3\n8 8\n255\n", w, h) == -1);
}

// A 16-bit maxval means two big-endian bytes per sample: a different pixel format. Reading it as
// 8-bit would show the high bytes as a dim, doubled image, so it is refused rather than guessed at.
TEST_CASE("VideoService PPM: a 16-bit maxval is rejected rather than misread as 8-bit") {
    uint16_t w = 0, h = 0;
    CHECK(parse("P6\n8 8\n65535\n", w, h) == -1);
}

// Garbage, an empty buffer, and a header cut off mid-token must all fail cleanly: the file source
// is fed by whatever the user uploads, so this is the ordinary case, not the exceptional one.
TEST_CASE("VideoService PPM: malformed and truncated headers fail without reading past the buffer") {
    uint16_t w = 0, h = 0;
    CHECK(parse("", w, h) == -1);
    CHECK(parse("not an image at all", w, h) == -1);
    CHECK(parse("P6", w, h) == -1);             // magic only
    CHECK(parse("P6\n64", w, h) == -1);         // no height
    CHECK(parse("P6\n64 36\n", w, h) == -1);    // no maxval
    CHECK(parse("P6\n64 36\n255", w, h) == -1); // no separator, so no pixel data can follow
}

// A zero side has no pixels, and an absurd dimension would overflow the width*height*3 allocation
// size. Both are refused at the header rather than at the allocation.
TEST_CASE("VideoService PPM: zero and out-of-range dimensions are refused at the header") {
    uint16_t w = 0, h = 0;
    CHECK(parse("P6\n0 36\n255\n", w, h) == -1);
    CHECK(parse("P6\n64 0\n255\n", w, h) == -1);
    CHECK(parse("P6\n99999 36\n255\n", w, h) == -1); // past kMaxDim
}

// With no service instantiated, latestFrame() still returns a readable struct: an effect must
// never have to null-check the POINTER, only the frame's contents. This is the no-source state
// every device is in before a capture source is added, so it has to be the safe one.
TEST_CASE("VideoService: latestFrame is readable with no service present and reports no frame") {
    const mm::VideoFrame* f = VideoService::latestFrame();
    REQUIRE(f != nullptr);
    CHECK(f->rgb == nullptr);
    CHECK(f->seq == 0);
    CHECK(f->width == 0);
    CHECK(f->height == 0);
}

// Deleting the elected source while a second one is still running must hand the seat over, not go
// permanently dark. The seat is vacated by the destructor, and a running module re-claims an empty
// one on its next tick, so effects keep seeing a live frame for any add/remove order. Same
// robustness AudioService's mic seat has; without the tick() re-claim only a reboot recovers.
TEST_CASE("VideoService: a survivor takes over the seat when the elected source is destroyed") {
    auto* elected = new VideoService(); // constructed first, so it claims the seat
    elected->source = VideoService::kSourcePattern; // needs no file
    elected->applyState();
    REQUIRE(VideoService::latestFrame()->rgb != nullptr);

    VideoService survivor; // seat already held, so its claim is a no-op
    survivor.source = VideoService::kSourcePattern;
    survivor.applyState();

    delete elected; // ~ActiveInstance vacates: the seat is now empty
    CHECK(VideoService::latestFrame()->rgb == nullptr);

    survivor.tick(); // the survivor inherits it
    CHECK(VideoService::latestFrame()->rgb != nullptr);
}

// Unit tests link the desktop platform, which cannot capture, so this pins that side: the usb
// source is not offered, and a config restored from a board that had one falls back rather than
// selecting a dead option. The static_assert fails loudly if the suite ever runs somewhere that
// CAN capture, which would need its own case rather than this one quietly changing meaning.
TEST_CASE("VideoService: a platform that cannot capture does not offer the usb source") {
    static_assert(!mm::platform::hasUsbVideo, "tests assume the desktop platform");
    CHECK(VideoService::kSourceCount == 2);

    VideoService v;
    v.source = VideoService::kSourceUsb;
    v.applyState();
    CHECK(v.source == VideoService::kSourcePattern); // fell back
    CHECK(VideoService::latestFrame()->rgb != nullptr);
}

// The format dropdown is populated from whatever the device advertises, so a platform with no
// capture at all must report an EMPTY list rather than a placeholder: VideoService only offers the
// control when the count is non-zero, and a phantom entry would let the user pick a dead format.
TEST_CASE("VideoService: a platform with no capture advertises no formats") {
    mm::platform::VideoCaptureFormat formats[4];
    CHECK(mm::platform::videoCaptureFormats(formats, 4) == 0);
}

// Accepting a non-whitespace separator eats a pixel and shifts every channel one place, which
// tints the whole image rather than failing.
TEST_CASE("VideoService PPM: the separator must be whitespace, not merely present") {
    uint16_t w = 0, h = 0;
    CHECK(parse("P6\n2 2\n255X", w, h) == -1);
    CHECK(parse("P6\n2 2\n255\n", w, h) == 11);   // the same header with a real separator
}
