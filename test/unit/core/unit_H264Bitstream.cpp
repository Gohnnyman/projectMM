/// @module H264Bitstream
/// @also RtspDriver

/// Where one encoded frame ends and the next begins. An Annex B stream carries no frame delimiter, so a reader that gets this wrong ships half a picture, which a decoder shows as a tear rather than reporting.

#include "doctest.h"
#include "core/util/H264Bitstream.h"

#include <vector>

namespace {

/// A NAL with a 4-byte start code: `type` in the header, `firstMb` set for a picture's first slice.
std::vector<uint8_t> nal(uint8_t type, bool firstMb = true, size_t payload = 2) {
    std::vector<uint8_t> out{0, 0, 0, 1, static_cast<uint8_t>(0x60 | type)};
    out.push_back(firstMb ? 0x80 : 0x20);          // first_mb_in_slice zero, or a later macroblock
    for (size_t i = 0; i < payload; i++) out.push_back(0x11);
    return out;
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

}  // namespace

// A VCL slice at macroblock zero opens a frame; the same type past macroblock zero continues one.
TEST_CASE("H264Bitstream opens an access unit on a first slice alone") {
    const auto first = nal(1, true);
    const auto later = nal(1, false);
    CHECK(mm::h264::opensAccessUnit(first.data() + 4, first.size() - 4));
    CHECK_FALSE(mm::h264::opensAccessUnit(later.data() + 4, later.size() - 4));
}

// Parameter sets and SEI describe the frame that FOLLOWS, so a boundary taken at an SPS would cut that frame away from the sets that decode it.
TEST_CASE("H264Bitstream opens no access unit on a parameter set or SEI") {
    for (uint8_t type : {uint8_t{6}, uint8_t{7}, uint8_t{8}}) {
        const auto n = nal(type, true);
        CHECK_FALSE(mm::h264::opensAccessUnit(n.data() + 4, n.size() - 4));
    }
}

// An access unit starts at the SPS rather than the slice: an IDR delivered without its parameter sets makes a decoder report a missing PPS and show nothing.
TEST_CASE("H264Bitstream keeps the parameter sets with the frame they describe") {
    std::vector<uint8_t> buf;
    append(buf, nal(7));                      // SPS
    append(buf, nal(8));                      // PPS
    append(buf, nal(6));                      // SEI
    append(buf, nal(5));                      // the IDR those three describe
    const size_t second = buf.size();
    append(buf, nal(1));                      // the next frame, which ends the first

    std::vector<size_t> starts;
    mm::h264::findAccessUnits(buf.data(), buf.size(), &starts);
    REQUIRE(starts.size() == 2);
    CHECK(starts[0] == 0);                    // the SPS, never the IDR that follows it
    CHECK(starts[1] == second);

    // The frame carries all four NALs, which is what a decoder needs to show the picture.
    CHECK(mm::h264::hasKeyframe(buf.data(), second));
}

// The boundaries of a two-frame buffer, each frame preceded by the sets that describe it.
TEST_CASE("H264Bitstream finds each frame start in a multi-frame buffer") {
    std::vector<uint8_t> buf;
    append(buf, nal(7));                      // SPS, which opens frame one: it describes it
    append(buf, nal(8));                      // PPS
    append(buf, nal(5));                      // IDR: frame one
    const size_t secondFrame = buf.size();
    append(buf, nal(1));                      // frame two

    std::vector<size_t> starts;
    mm::h264::findAccessUnits(buf.data(), buf.size(), &starts);
    REQUIRE(starts.size() == 2);
    CHECK(starts[0] == 0);
    CHECK(starts[1] == secondFrame);
}

// A multi-slice picture is ONE frame: only its first slice sits at macroblock zero, so the rest must not each be read as a new one.
TEST_CASE("H264Bitstream reads a multi-slice picture as one access unit") {
    std::vector<uint8_t> buf;
    append(buf, nal(1, true));                // the picture's first slice
    append(buf, nal(1, false));               // three more slices of the SAME picture
    append(buf, nal(1, false));
    append(buf, nal(1, false));

    std::vector<size_t> starts;
    mm::h264::findAccessUnits(buf.data(), buf.size(), &starts);
    CHECK(starts.size() == 1);
}

// A buffer cut mid-NAL reports only what is whole: the trailing bytes are a frame still arriving, and publishing them would ship a torn picture.
TEST_CASE("H264Bitstream ignores a trailing partial NAL") {
    std::vector<uint8_t> buf;
    append(buf, nal(5));
    buf.insert(buf.end(), {0, 0, 0, 1});      // a start code whose header byte has yet to arrive

    std::vector<size_t> starts;
    mm::h264::findAccessUnits(buf.data(), buf.size(), &starts);
    CHECK(starts.size() == 1);                // the complete frame, never the truncated one
}

// An IDR is what a joining client decodes from, so a frame is reported as a keyframe on that alone.
TEST_CASE("H264Bitstream reports a keyframe by its IDR NAL") {
    const auto idr = nal(5);
    const auto inter = nal(1);
    CHECK(mm::h264::hasKeyframe(idr.data(), idr.size()));
    CHECK_FALSE(mm::h264::hasKeyframe(inter.data(), inter.size()));
}

// Annex B allows a 3-byte start code as well as a 4-byte one, and one encoder emits both.
TEST_CASE("H264Bitstream reads three-byte and four-byte start codes alike") {
    std::vector<uint8_t> buf{0, 0, 1, 0x65, 0x80, 0x11,      // a 3-byte coded IDR
                             0, 0, 0, 1, 0x61, 0x80, 0x11};  // then a 4-byte coded slice
    std::vector<size_t> starts;
    mm::h264::findAccessUnits(buf.data(), buf.size(), &starts);
    REQUIRE(starts.size() == 2);
    CHECK(starts[0] == 0);
    CHECK(starts[1] == 6);
}

// An empty or undersized buffer answers rather than reading past its end.
TEST_CASE("H264Bitstream answers an empty buffer without reading past it") {
    std::vector<size_t> starts;
    mm::h264::findAccessUnits(nullptr, 0, &starts);
    const uint8_t two[2] = {0, 0};
    mm::h264::findAccessUnits(two, sizeof(two), &starts);
    CHECK(starts.empty());
    CHECK_FALSE(mm::h264::opensAccessUnit(two, 1));
    CHECK_FALSE(mm::h264::hasKeyframe(nullptr, 10));
}
