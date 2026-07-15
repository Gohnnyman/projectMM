// @module MoonI80LedDriver
// @also ParallelLedDriver

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"
#include "correction_presets.h"

#include <cstring>
#include <vector>

// The DOMAIN half of the MoonI80 streaming ring (the platform GDMA half is bench-verified on the S3 —
// desktop stubs return false, so busIsRing() is false on host and tick() never routes here). What this
// pins is the part where the prior ring attempt "rendered wrong": the slice tiling the platform's refill
// task drives through the encode seam. The invariants:
//
//   1. TILING — the slices, concatenated in frame-row order, are byte-identical to one whole-frame
//      encode. (A slice writes to dst+0 for any firstRow, so a tiling bug shows as a shifted/duplicated
//      block — exactly the "domino" artifact of the stashed attempt.)
//   2. LAST-SLICE-CLOSES — only the final slice appends the latch pad; the others must not.
//   3. RECYCLED == FRESH — driving the same buffers a SECOND time yields identical bytes. Ring buffers
//      are recycled, not zeroed, so a stale-constant bug (the prefill/pad not re-laid) shows only on the
//      second frame. This is the invariant most likely to break, and the one no whole-frame test covers.
//
// The mock's "ring" is plain memory driven exactly as platform_esp32_moon_i80.cpp drives it: prime N
// buffers with the first N slices, then refill in ring order until the last slice — so the sequence the
// encode seam sees here is the sequence it sees on hardware.

namespace {

using mm::nrOfLightsType;

// Deliberately SMALLER than the platform's kRingBufs (8): a 4-buffer mock forces buffer REUSE at fewer
// slices (a 200-light frame = 13 slices reuses buffers 0..4), which is exactly the path the recycled /
// short-last-slice tests need to exercise. The mock tests the domain SLICING contract, not the depth.
constexpr uint8_t kMockRingBufs = 4;
constexpr uint32_t kMockRingRows = 16; // mirrors kRingRows

class MockRingDriver : public mm::ParallelLedDriver<MockRingDriver> {
public:
    static constexpr uint8_t lanesAvailable() { return 8; }   // 8 data lines (an 8-bit bus)
    static constexpr bool kPowerOfTwoBus = true;
    static constexpr bool kLoopbackFullWidth = false;
    static constexpr bool kSupportsShiftRegister = true;
    static constexpr const char* kInitFailMsg = "mock init failed";

    void addBusControls() {}
    bool busControlTriggersBuild(const char*) const { return false; }
    void recordBusPins() {}
    bool extraBusPinsCurrent() const { return true; }
    const char* validateBusPins(const uint16_t*, uint8_t) const { return nullptr; }
    const char* validateBusFatal() const { return nullptr; }
    uint16_t clockPinForBus() const { return 99; }

    // --- whole-frame bus (used to produce the reference frame the ring output is compared against) ---
    bool busInit(size_t frameBytes, bool) { cap_ = frameBytes; buf_.assign(frameBytes, 0); return true; }
    uint8_t* busBuffer(uint8_t i) { return (i == 0 && !buf_.empty()) ? buf_.data() : nullptr; }
    size_t busCapacity() const { return cap_; }
    bool busTransmit(uint8_t, size_t) { return true; }
    bool busWait(uint8_t, uint32_t) { return true; }
    uint32_t busLastTransmitUs() const { return 0; }
    void busDeinit() { cap_ = 0; buf_.clear(); ringActive_ = false; }
    mm::platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) { return {}; }

    // The whole-frame reference: prefill constants + encode the entire frame in one call (what the ring
    // must reproduce, slice by slice).
    template <class Slot>
    void encodeWholeForTest(uint8_t outCh, uint8_t* dst) {
        this->template encodeRows<Slot>(outCh, dst, 0, 0, /*closeFrame=*/true);
    }
    template <class Slot>
    void prefillShiftFrameForTest(uint8_t outCh, uint8_t* dst) {
        this->template prefillShiftFrame<Slot>(outCh, dst);
    }

    // --- ring hooks (the seam the platform drives). The mock stores the trampoline + geometry and hands
    //     out plain-memory buffers; driveRingFrame() below replays the platform's prime+refill order. ---
    bool wantsRing() const { return wantRing_; }
    void setWantRing(bool w) { wantRing_ = w; }
    bool busInitRing(size_t rowBytes, uint32_t totalRows, size_t padBytes) {
        ringRowBytes_ = rowBytes;
        ringPad_ = padBytes;
        ringTotalRows_ = totalRows;
        ringActive_ = true;
        const size_t bufBytes = static_cast<size_t>(kMockRingRows) * rowBytes + padBytes;
        for (auto& b : ring_) b.assign(bufBytes, 0);
        return true;
    }
    bool busIsRing() const { return ringActive_; }
    bool busTransmitRing() { return true; }   // the wire itself is the platform's; the encode is what we test

    // Replay one frame through the ring exactly as the platform does: prime the first min(N, needed)
    // buffers, then refill in ring order until the slice that reaches totalRows. Returns the frame
    // reassembled in ROW order (buffer contents concatenated in the order the DMA would clock them),
    // WITHOUT the pad, so it lines up with a whole-frame encode's row region for a byte compare.
    std::vector<uint8_t> driveRingFrame() {
        REQUIRE(ringActive_);
        std::vector<uint8_t> assembled;
        uint32_t row = 0;
        uint8_t slot = 0;
        int32_t lastSlot = -1;
        // The row region of one buffer (pad excluded), in bytes, for a `count`-row slice.
        auto rowBytesOf = [&](uint32_t count) { return static_cast<size_t>(count) * ringRowBytes_; };
        while (row < ringTotalRows_) {
            uint32_t count = kMockRingRows;
            bool last = false;
            if (row + count >= ringTotalRows_) { count = ringTotalRows_ - row; last = true; }
            // The platform calls the trampoline with (dst, firstRow, count, closeFrame=last).
            MockRingDriver::ringEncodeTrampolineHost(this, ring_[slot].data(), row, count, last);
            // Reassemble the row region in DMA order.
            assembled.insert(assembled.end(), ring_[slot].begin(),
                             ring_[slot].begin() + static_cast<long>(rowBytesOf(count)));
            row += count;
            if (last) lastSlot = slot;
            slot = (slot + 1u) % kMockRingBufs;
        }
        lastSlot_ = lastSlot;
        return assembled;
    }

    // Was the latch pad written in the LAST slice's buffer (and nowhere else)? Checks the pad region of
    // every ring buffer: exactly the last-slice buffer may be non-zero there.
    bool onlyLastSliceClosedFrame() const {
        for (uint8_t s = 0; s < kMockRingBufs; s++) {
            const size_t rowRegion = static_cast<size_t>(kMockRingRows) * ringRowBytes_;
            bool padNonZero = false;
            for (size_t i = rowRegion; i < ring_[s].size(); i++)
                if (ring_[s][i] != 0) { padNonZero = true; break; }
            // Only the buffer that held the last slice may have a written pad. (Other buffers were also
            // used for earlier slices, but those were encoded with closeFrame=false → pad stays zero.)
            if (padNonZero && static_cast<int32_t>(s) != lastSlot_) return false;
        }
        return true;
    }

    // After driveRingFrame(), the LAST slice's buffer must have a clean pad: past its (possibly short)
    // row region, only the ONE latch word may be non-zero — the ≥300 µs LOW reset the DMA clocks at
    // frame end. This is what a non-multiple-of-16 strand length (a short last slice into a REUSED
    // buffer) breaks without the pad-zero: the buffer's earlier full slice would still sit in the pad.
    // Returns the count of non-zero bytes in the pad window BEYOND the first latch word — 0 = clean.
    size_t lastSliceStalePadBytes() const {
        if (lastSlot_ < 0) return SIZE_MAX;
        const size_t lastRows = ringTotalRows_ - (nSlicesForTest() - 1) * kMockRingRows;
        const size_t rowRegion = lastRows * ringRowBytes_;   // where this slice's pad window begins
        const auto& b = ring_[static_cast<size_t>(lastSlot_)];
        // Only the DMA-VISIBLE window matters: the last node is mounted at lastRows*rowBytes + padBytes,
        // so the DMA clocks [0, rowRegion + ringPad_) and NOTHING beyond it — bytes past that (the tail of
        // an oversized recycled buffer) are never on the wire. Within the visible pad, only the first
        // latch word may be non-zero; count any OTHER non-zero byte in that window.
        const size_t visibleEnd = rowRegion + ringPad_;
        size_t stale = 0;
        for (size_t i = rowRegion + slotBytesForTest(); i < visibleEnd && i < b.size(); i++)
            if (b[i] != 0) stale++;
        return stale;
    }
    uint32_t nSlicesForTest() const {
        return (ringTotalRows_ + kMockRingRows - 1) / kMockRingRows;
    }
    size_t slotBytesForTest() const { return this->slotBytes(); }

    // The trampoline the real driver registers is MoonI80LedDriver::ringEncodeTrampoline; the mock
    // reproduces its body (recover `this`, branch on bus width, call encodeRows) so the host drives the
    // identical encode the seam does on device.
    static void ringEncodeTrampolineHost(void* user, uint8_t* dst, uint32_t firstRow,
                                         uint32_t count, bool closeFrame) {
        auto* self = static_cast<MockRingDriver*>(user);
        const uint8_t outCh = self->correction_.outChannels;
        const auto first = static_cast<nrOfLightsType>(firstRow);
        const auto cnt = static_cast<nrOfLightsType>(count);
        // Zero the pad window before a LAST slice into a recycled buffer — mirrors encodeRingSlice in the
        // platform. A short last slice (count < kMockRingRows) leaves this buffer's EARLIER full slice in
        // the pad region; without this the encoder's "rest of the pad is zero" contract breaks and ghost
        // rows clock in place of the reset. (This is the fix for the non-multiple-of-16 stale-pad bug.)
        if (closeFrame && self->ringPad_) {
            std::memset(dst + static_cast<size_t>(count) * self->ringRowBytes_, 0, self->ringPad_);
        }
        // Prefill THEN encode, exactly as MoonI80LedDriver::ringEncodeTrampoline does — the recycled
        // buffer needs its constants re-laid, and encodeRows writes only the data word in shift mode.
        if (self->slotBytes() == 1) {
            if (self->shiftMode()) self->template prefillShiftRows<uint8_t>(outCh, dst, first, cnt);
            self->template encodeRows<uint8_t>(outCh, dst, first, cnt, closeFrame);
        } else {
            if (self->shiftMode()) self->template prefillShiftRows<uint16_t>(outCh, dst, first, cnt);
            self->template encodeRows<uint16_t>(outCh, dst, first, cnt, closeFrame);
        }
    }

    size_t rowBytesForTest() const { return ringRowBytes_; }

private:
    std::vector<uint8_t> buf_;
    size_t cap_ = 0;
    std::vector<uint8_t> ring_[kMockRingBufs];
    size_t ringRowBytes_ = 0;
    size_t ringPad_ = 0;
    uint32_t ringTotalRows_ = 0;
    bool ringActive_ = false;
    bool wantRing_ = false;
    int32_t lastSlot_ = -1;
};

// Bring the mock up on `lights` lights, shift mode (8 pins × 8 = 64 strands... capped; use fewer pins),
// with a correction so outChannels is known. Mirrors the shiftregister test's setup.
void wireShift(MockRingDriver& d, mm::Buffer& src, mm::Correction& corr, nrOfLightsType lights,
               const char* pins) {
    std::strcpy(d.pins, pins);
    d.shiftRegister = true;
    d.latchPin = 20;
    REQUIRE(src.allocate(lights * 8, 3) == true);   // enough lights for the strands
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

// 1. TILING — the ring's slices, concatenated in row order, reproduce a whole-frame encode byte for
//    byte. This is the invariant the prior attempt violated (the 16-stride "domino" repeat). A slice
//    writes to dst+0 for any firstRow, so if the tiling is right the reassembled buffer == the frame.
TEST_CASE("MoonI80 ring: sliced encode tiles into a byte-identical whole frame") {
    MockRingDriver d;
    mm::Buffer src;
    mm::Correction corr;
    // 200 lights/strand > 16 rows/buffer × 4 buffers = 64, so this needs real refills (not fits-in-ring).
    wireShift(d, src, corr, 200, "1,2");   // 2 pins × 8 = 16 strands, 200 lights each
    // Paint the source so every row differs (a tiling bug that repeats a slice would then be visible).
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 7);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 13 + 1);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 29 + 2);
    }
    const uint8_t outCh = corr.outChannels;
    // The per-row encoded size (8-bit bus → slotBytes 1; the '595 multiplies the slot count).
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    const size_t padBytes = static_cast<size_t>(800 + 64) * 1 * d.outputsPerPin();
    const size_t rowRegion = static_cast<size_t>(d.maxLaneLights()) * rowBytes;

    // Reference: one whole-frame encode (2 pins → 8-bit bus → uint8 slots). encodeRows in shift mode
    // writes only data words, so prefill the whole buffer's constants first — same as reinit does.
    d.busInit(d.frameBytes(), false);
    d.prefillShiftFrameForTest<uint8_t>(outCh, d.busBuffer(0));
    d.encodeWholeForTest<uint8_t>(outCh, d.busBuffer(0));
    std::vector<uint8_t> whole(d.busBuffer(0), d.busBuffer(0) + rowRegion);
    d.busDeinit();

    // Ring: drive the frame slice by slice, reassemble the row region.
    d.setWantRing(true);
    REQUIRE(d.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights()), padBytes));
    std::vector<uint8_t> assembled = d.driveRingFrame();

    REQUIRE(assembled.size() == whole.size());
    CHECK(std::memcmp(assembled.data(), whole.data(), whole.size()) == 0);
}

// 2. LAST-SLICE-CLOSES — only the final slice writes the latch pad; every earlier slice leaves its
//    buffer's pad region zero. A frame that closed early (or on every slice) would latch mid-frame.
TEST_CASE("MoonI80 ring: only the last slice closes the frame") {
    MockRingDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, src, corr, 200, "1,2");
    const uint8_t outCh = corr.outChannels;

    d.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    const size_t padBytes = static_cast<size_t>(800 + 64) * 1 * d.outputsPerPin();
    REQUIRE(d.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights()), padBytes));
    d.driveRingFrame();
    CHECK(d.onlyLastSliceClosedFrame());
}

// 3. RECYCLED == FRESH — a second frame through the SAME (recycled, not zeroed) ring buffers produces
//    byte-identical output to the first. This catches a stale-constant / stale-pad bug that a
//    single-frame test cannot see — the failure mode unique to a recycled ring.
TEST_CASE("MoonI80 ring: a recycled buffer produces the same bytes as a fresh one") {
    MockRingDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, src, corr, 200, "1,2");
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 5 + 3);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 11);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 17 + 7);
    }
    const uint8_t outCh = corr.outChannels;

    d.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    const size_t padBytes = static_cast<size_t>(800 + 64) * 1 * d.outputsPerPin();
    REQUIRE(d.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights()), padBytes));

    std::vector<uint8_t> first = d.driveRingFrame();
    std::vector<uint8_t> second = d.driveRingFrame();   // same buffers, recycled — not re-init'd
    REQUIRE(first.size() == second.size());
    CHECK(std::memcmp(first.data(), second.data(), first.size()) == 0);
}

// 4. NON-MULTIPLE-OF-16 STRAND LENGTH — the last slice is SHORT and lands in a REUSED buffer, so its
//    pad window still holds that buffer's earlier full slice. The pad-zero must scrub it, or ghost rows
//    clock in place of the ≥300 µs LOW reset and a strand can miss its latch. 200 lights → 13 slices >
//    8 buffers (reuse) AND lastRows=8 (short) — exactly the case 128/192/256 (all ×16) never hit.
TEST_CASE("MoonI80 ring: a short last slice in a reused buffer has a clean pad (no stale ghost rows)") {
    MockRingDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, src, corr, 200, "1,2");   // 200 lights/strand: 13 slices, last slice = 8 rows
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {   // dense non-zero source so a stale row WOULD show
        s[i * 3 + 0] = static_cast<uint8_t>(i * 9 + 1);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 3 + 5);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 19 + 2);
    }
    const uint8_t outCh = corr.outChannels;
    d.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    const size_t padBytes = static_cast<size_t>(800 + 64) * 1 * d.outputsPerPin();
    REQUIRE(d.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights()), padBytes));

    // Drive TWICE so the last-slice buffer is genuinely recycled (held an earlier frame's full slice).
    d.driveRingFrame();
    d.driveRingFrame();
    CHECK(d.lastSliceStalePadBytes() == 0);   // pad clean past the one latch word — no ghost rows
}
