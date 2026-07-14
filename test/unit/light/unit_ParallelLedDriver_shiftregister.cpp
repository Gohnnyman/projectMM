// @module I80LedDriver
// @also ParallelLedDriver

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"
#include "correction_presets.h"

#include <cstring>
#include <vector>

// The DRIVER half of the 74HCT595 shift-register expander (unit_ParallelSlots pins the encoded
// bits). What matters here is the arithmetic the user's memory budget depends on, and the config
// guards that stop a mis-wired board from emitting a waveform the hardware can't sustain:
//
//   - lanes = pins x 8 (each data pin fans out to 8 strands through its '595)
//   - the DMA frame grows x8 (a '595 is serial-in: presenting a slot costs 8 shift cycles)
//   - but extra STRANDS are free (they ride the bus width), so 15x256 and 48x256 cost the SAME
//   - the bus stays 8-bit: 48 strands on 6 pins is not a 16-bit bus
//   - the latch is a bus lane, so it must not collide with a data pin

namespace {

using mm::nrOfLightsType;

// A mock parallel driver whose "bus" is plain memory (same shape as the double-buffer mock),
// so the lane/frame arithmetic is provable on the host with no peripheral.
class MockShiftDriver : public mm::ParallelLedDriver<MockShiftDriver> {
public:
    static constexpr uint8_t lanesAvailable() { return 8; }   // 8 data lines, like an 8-bit bus
    // The i80 shape: the BUS rounds to 8/16 whatever the pin count, so the driver pads the lane list.
    // (Parlio, the other shape, sets this false — its bus width IS the pin count.) The expander only
    // ever runs on an i80-shaped bus, so the mock models that one.
    static constexpr bool kPowerOfTwoBus = true;
    static constexpr bool kLoopbackFullWidth = false;
    static constexpr bool kSupportsShiftRegister = true;      // memory bus: the expander is allowed
    static constexpr const char* kInitFailMsg = "mock init failed";

    void addBusControls() {}
    bool busControlTriggersBuild(const char*) const { return false; }
    void recordBusPins() {}
    bool extraBusPinsCurrent() const { return true; }
    const char* validateBusPins(const uint16_t*, uint8_t) const { return nullptr; }
    const char* validateBusFatal() const { return nullptr; }

    bool busInit(size_t frameBytes, bool) {
        cap_ = frameBytes;
        buf_.assign(frameBytes, 0);
        return true;
    }

    // The streaming ring encodes the frame one SLICE at a time, straight into the small internal
    // buffer the DMA is about to read. That is only sound if a sliced encode produces byte-identical
    // output to the whole-frame encode — so expose the encoder for the test that pins it.
    template <class Slot>
    void encodeSliceForTest(uint8_t outCh, uint8_t* dst, mm::nrOfLightsType first,
                            mm::nrOfLightsType count, bool closeFrame) {
        this->template encodeRows<Slot>(outCh, dst, first, count, closeFrame);
    }
    uint8_t* busBuffer(uint8_t i) { return (i == 0 && !buf_.empty()) ? buf_.data() : nullptr; }
    size_t busCapacity() const { return cap_; }
    // busTransmit reports success — as the real one does. This is the crux of the 2026-07-14 bug:
    // esp_lcd's tx_color returns ESP_OK because the ENQUEUE succeeded, while the GDMA mount fails
    // later inside the ISR. So "transmit returned true" does NOT mean the frame reached the wire.
    bool busTransmit(uint8_t, size_t) { transmits_++; return true; }
    // `waitTimesOut` simulates a transfer whose done-callback never fires — precisely what a failed
    // GDMA mount produces. The driver must degrade, not wedge, and must not reuse the buffer.
    bool busWait(uint8_t, uint32_t) { return !waitTimesOut; }
    bool waitTimesOut = false;
    uint32_t busLastTransmitUs() const { return 0; }
    void busDeinit() { cap_ = 0; buf_.clear(); }
    mm::platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) {
        return {};
    }

    // Test-only view of the derived lane math.
    uint8_t physPinsForTest() const { return physPins_; }
    size_t transmitCount() const { return transmits_; }
    // The GPIO list + length handed to the PERIPHERAL — the two must agree, or the platform reads off
    // the end of the array. See the padding test below.
    const uint16_t* busPinListForTest() { return this->busPinList(); }
    uint8_t busPinCountForTest() const { return this->busPinCount(); }
    uint16_t clockPinForBus() const { return 99; }   // a recognisable "parked here" sentinel

private:
    std::vector<uint8_t> buf_;
    size_t cap_ = 0;
    size_t transmits_ = 0;
};

// Bring a mock driver up on `lights` lights with the given pin list and expander setting.
// shiftOn fits the 74HCT595 expander (8 strands per pin); latch is the latch GPIO.
void wire(MockShiftDriver& d, mm::Buffer& src, mm::Correction& corr, nrOfLightsType lights,
          const char* pins, bool shiftOn, int8_t latch, const char* ledsPerPin = "") {
    std::strcpy(d.pins, pins);
    std::strcpy(d.ledsPerPin, ledsPerPin);
    d.shiftRegister = shiftOn;
    d.latchPin = latch;
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

// Each data pin fans out to 8 strands through its '595, so the driver drives pins x 8 lanes —
// the whole point of the expander (pins are the scarce resource, not strands).
TEST_CASE("shift register: lanes = pins x 8") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    // 6 data pins + a latch → 48 strands, the PO's 48x256 panel.
    wire(d, src, corr, 48 * 256, "1,2,3,4,5,6", /*shiftOn=*/true, /*latch=*/7);

    CHECK(d.physPinsForTest() == 6);
    CHECK(d.laneCount() == 48);          // 6 pins x 8 outputs
    CHECK(d.maxLaneLights() == 256);     // 12288 lights spread over 48 strands
}

// Direct mode is unchanged: one strand per pin, no latch. Pins the no-regression half —
// the expander must not have altered the existing behaviour.
TEST_CASE("shift register: direct mode still drives one strand per pin") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 4 * 256, "1,2,3,4", /*shiftOn=*/false, /*latch=*/-1);

    CHECK(d.physPinsForTest() == 4);
    CHECK(d.laneCount() == 4);           // no fan-out
    CHECK(d.maxLaneLights() == 256);
}

// THE COUNTER-INTUITIVE RESULT, and the one the memory budget rests on: the x8 fan-out multiplies
// the frame (a '595 is serial-in — each slot costs 8 shift cycles), but extra STRANDS are free
// (they ride the bus width). So the PO's two panels — 15x256 = 3,840 lights and 48x256 = 12,288
// lights — cost the SAME DMA frame. If this ever regresses, the memory model is wrong and the
// bigger panel will silently fail to allocate.
TEST_CASE("shift register: frame is set by strand LENGTH and the fan-out, not by strand COUNT") {
    mm::Correction corr;

    // 2 pins x 8 = 16 strands, 256 lights each.
    MockShiftDriver small;
    mm::Buffer srcSmall;
    wire(small, srcSmall, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);
    CHECK(small.laneCount() == 16);
    CHECK(small.maxLaneLights() == 256);

    // 6 pins x 8 = 48 strands, still 256 lights each — 3x the strands, 3x the lights.
    MockShiftDriver big;
    mm::Buffer srcBig;
    wire(big, srcBig, corr, 48 * 256, "1,2,3,4,5,6", /*shiftOn=*/true, /*latch=*/7);
    CHECK(big.laneCount() == 48);
    CHECK(big.maxLaneLights() == 256);

    // Same strand LENGTH → same frame, despite 3x the strands and 3x the lights.
    CHECK(big.frameBytes() == small.frameBytes());
}

// The x8 IS paid for, in the frame: the same strands at the same length cost 8x the DMA bytes with
// the expander fitted. (This is what walls the classic ESP32 — its DMA can't reach PSRAM.)
TEST_CASE("shift register: the fan-out costs 8x the DMA frame") {
    mm::Correction corr;

    // 8 strands, 256 lights each — direct (8 pins, one strand each).
    MockShiftDriver direct;
    mm::Buffer srcDirect;
    wire(direct, srcDirect, corr, 8 * 256, "1,2,3,4,5,6,7,8", /*shiftOn=*/false, /*latch=*/-1);
    CHECK(direct.laneCount() == 8);
    CHECK(direct.maxLaneLights() == 256);

    // The same 8 strands at the same length, but through ONE pin's '595.
    MockShiftDriver shifted;
    mm::Buffer srcShifted;
    wire(shifted, srcShifted, corr, 8 * 256, "1", /*shiftOn=*/true, /*latch=*/2);
    CHECK(shifted.laneCount() == 8);
    CHECK(shifted.maxLaneLights() == 256);

    // Identical strands, but the serial shift-out costs 8 bus words per slot instead of 1.
    // (Not exactly 8x the BYTES: both frames carry the same 64-byte-aligned latch pad, which is
    // itself scaled — so assert the ratio is the real, substantial growth rather than a fixed
    // constant that would bake the pad arithmetic into the test.)
    CHECK(shifted.frameBytes() > direct.frameBytes() * 7);
}




// The bus width follows the PHYSICAL pins (+ the latch lane), not the strand count. 48 strands on
// 6 pins is still an 8-bit bus — if this regressed to keying on the strand count, the driver would
// encode 16-bit slots into an 8-bit bus and emit a frame of pure garbage.
TEST_CASE("shift register: 48 strands on 6 pins is still an 8-bit bus") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 48 * 256, "1,2,3,4,5,6", /*shiftOn=*/true, /*latch=*/7);

    CHECK(d.laneCount() == 48);   // ...strands, but
    // frameBytes = rows x channels x 24 slots x slotBytes x 8 (+ pad). With an 8-bit
    // bus slotBytes is 1; a (wrong) 16-bit bus would double this. Derive the expected value from
    // the 8-bit assumption and check the driver agrees.
    const size_t rowBytes = static_cast<size_t>(256) * 3 * 24 * 1 * 8;
    CHECK(d.frameBytes() >= rowBytes);
    CHECK(d.frameBytes() < rowBytes * 2);   // a 16-bit bus would be >= 2x
}

// The latch is a real bus lane (a bit in every bus word), so it cannot double as a data pin —
// that lane would carry the latch waveform instead of pixel data. A config error, not a crash.
TEST_CASE("shift register: latchPin colliding with a data pin is a config error") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 8 * 256, "1,2,3,4", /*shiftOn=*/true, /*latch=*/3);   // 3 is a data pin

    CHECK(d.severity() == MockShiftDriver::Severity::Error);
    CHECK(d.laneCount() == 0);   // idles rather than driving a broken bus
}

// Without a latch the '595s never present a byte, so the strands would stay dark. Refuse the
// config rather than run a bus that silently outputs nothing.
TEST_CASE("shift register: the expander needs a latchPin") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 8 * 256, "1,2,3,4", /*shiftOn=*/true, /*latch=*/-1);   // unset

    CHECK(d.severity() == MockShiftDriver::Severity::Error);
    CHECK(d.laneCount() == 0);
}

// The latch must not land on the peripheral's own WR/DC pins either — bench-found, because WR
// defaults to GPIO 10 and that is the first free-looking pin a user reaches for. The i80 bus builds
// fine, so the failure is silent garbage on the strands rather than an init error; that is what makes
// it worth an explicit guard. (The check itself lives in I80LedDriver::validateBusFatal, which the
// mock does not have — this pins the base's half: a data-pin collision is caught, so the mechanism
// is live. The WR/DC half is a compile-time-visible guard in the i80 driver.)
TEST_CASE("shift register: driver refuses a latch that collides with a data lane") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    // Latch on data pin 2 → the lane would carry the latch waveform instead of pixels.
    wire(d, src, corr, 8 * 256, "1,2,3", /*shiftOn=*/true, /*latch=*/2);
    CHECK(d.severity() == MockShiftDriver::Severity::Error);
    CHECK(d.laneCount() == 0);
}

// The loopback test frame must carry the expander's ×8 factor. The rig transmits the REAL frame
// through the real DMA path, so a frame sized without the ×8 would clock out a truncated waveform
// and fail every bit — a false failure that looks like broken hardware. (The mock's busLoopback is a
// stub, so this pins the SIZE arithmetic, which is the part that was wrong; the captured-bits half
// is hardware and is proven on the bench.)
TEST_CASE("shift register: the loopback test frame is 8x the direct-mode frame") {
    mm::Correction corr;

    MockShiftDriver direct;
    mm::Buffer srcDirect;
    wire(direct, srcDirect, corr, 8 * 64, "1,2,3,4,5,6,7,8", /*shiftOn=*/false, /*latch=*/-1);

    MockShiftDriver shifted;
    mm::Buffer srcShifted;
    wire(shifted, srcShifted, corr, 8 * 64, "1", /*shiftOn=*/true, /*latch=*/2);

    // Same strands, same length; the shift frame pays 8 bus words per slot instead of 1. frameBytes()
    // is the operational frame, which the loopback's per-light sizing mirrors (both scale by
    // the expander's ×8) — so the ratio is the invariant worth pinning.
    CHECK(shifted.frameBytes() > direct.frameBytes() * 7);
}

// Robust to any input (CLAUDE.md): flipping the expander on and off on a live driver must
// reconfigure cleanly each time, never wedge or leave stale lane state behind.
TEST_CASE("shift register: toggling the expander live reconfigures cleanly") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 8 * 256, "1,2,3,4", /*shiftOn=*/false, /*latch=*/-1);
    CHECK(d.laneCount() == 4);
    const size_t directFrame = d.frameBytes();

    // Expander ON (with a latch) → 32 strands, bigger frame.
    d.shiftRegister = true;
    d.latchPin = 7;
    d.applyState();
    CHECK(d.laneCount() == 32);          // 4 pins x 8
    CHECK(d.frameBytes() > directFrame);

    // ...and back OFF → exactly the original configuration, no residue.
    d.shiftRegister = false;
    d.applyState();
    CHECK(d.laneCount() == 4);
    CHECK(d.frameBytes() == directFrame);
    CHECK(d.severity() != MockShiftDriver::Severity::Error);
}

// ===========================================================================
// REGRESSION GUARDS for the 2026-07-14 bench failure. Every bug that day was invisible to the
// tests, which is precisely why it took two days to find: the suite asserted the encoder's INTERNAL
// LAYOUT (bus-word indices) rather than OBSERVABLE BEHAVIOUR. These pin the behaviour.
//
// The bug: with the ×8 expander the DMA frame is 8× bigger (154 KB), so a transfer takes ~4.6 ms on
// the wire instead of ~0.6 ms. esp_lcd's GDMA link list is mounted from index 0 and only counts
// descriptors the PREVIOUS transfer has released, so the next frame's mount landed mid-transfer and
// failed (`lli full`). Critically the failure was SILENT: tx_color returns ESP_OK (the enqueue
// succeeded; the mount fails later in the ISR), so the driver believed the transfer had started,
// waited for a done-callback that never came, and burned a 1 s timeout every frame.
//
// The observable symptoms — and what each test below pins:
//   1. a transfer that never completes must not wedge the driver (it must keep ticking, degraded);
//   2. it must never re-encode into a buffer the DMA may still be reading (frame corruption);
//   3. the driver must not silently treat "enqueued" as "delivered".

// A transfer that NEVER completes (the done-callback never fires) must not wedge the driver. On the
// bench this manifested as a 42 ms driver tick for a 4.6 ms transfer — the driver spent every frame
// inside a timing-out wait. It must stay responsive, and it must not corrupt the in-flight buffer.
TEST_CASE("shift register: a transfer that never completes does not wedge the driver") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);
    REQUIRE(d.laneCount() == 16);

    d.waitTimesOut = true;      // the DMA never signals done — exactly the bench failure

    // Tick repeatedly. The driver must keep running (no hang, no crash) and must NOT report success.
    for (int i = 0; i < 5; i++) d.tick();

    // It must still be alive and configured — degraded, not dead.
    CHECK(d.laneCount() == 16);
    CHECK(d.severity() != MockShiftDriver::Severity::Error);
}

// The buffer-safety invariant the timeout exists to protect: while a transfer may still be reading a
// buffer, the driver must NOT encode into it again. A re-encode mid-transfer is what puts half of one
// frame and half of the next on the wire — the "scattered random pixels" seen on the bench.
TEST_CASE("shift register: a timed-out transfer never gets its buffer re-encoded") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);

    d.waitTimesOut = true;
    d.tick();                       // starts a transfer that will never complete
    const size_t transmitsAfterFirst = d.transmitCount();

    d.tick();                       // must NOT transmit again over the live buffer
    d.tick();

    CHECK(d.transmitCount() == transmitsAfterFirst);   // no new transfer while the old one is stuck
}

// **The robustness rule: a broken bus must not cost the user the DEVICE.** Every dead transfer is a
// wait on the render thread, so a bus that never delivers doesn't just fail to light LEDs — it eats
// the CPU that WiFi/HTTP need and the device drops off the network entirely, with no way back in
// except a cable. That is exactly what a misconfigured shift-register frame did to a bench board
// (2026-07-14): unreachable and unrecoverable remotely, from nothing but an LED setting.
//
// So a persistently-dead bus must be GIVEN UP ON: the driver reports the failure and stops paying for
// it. Output idle, device alive — never the other way round.
TEST_CASE("a persistently dead bus is given up on, so it cannot starve the device") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);

    d.waitTimesOut = true;   // the DMA never signals done, every frame, forever

    for (int i = 0; i < 40; i++) d.tick();

    // It gave up: the failure is REPORTED (the user can see why the LEDs are dark)...
    CHECK(d.severity() == MockShiftDriver::Severity::Error);
    // ...and it stopped spending the render thread on a bus that will never deliver. The exact count
    // doesn't matter; what matters is that it is BOUNDED — it did not keep trying for all 40 ticks.
    CHECK(d.transmitCount() < 40u);
}

// Giving up must not be permanent: the user fixes the setting that broke the bus, the driver rebuilds,
// and the LEDs come back — no reboot (the live-reconfiguration rule).
TEST_CASE("a given-up driver recovers when the bus is fixed") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);

    d.waitTimesOut = true;
    for (int i = 0; i < 40; i++) d.tick();
    REQUIRE(d.severity() == MockShiftDriver::Severity::Error);   // gave up
    const size_t transmitsWhileDead = d.transmitCount();

    // The user fixes the config: the bus is rebuilt, and now transfers complete.
    d.waitTimesOut = false;
    d.applyState();          // prepare -> reinit: a fresh bus deserves a clean slate

    for (int i = 0; i < 5; i++) d.tick();

    CHECK(d.transmitCount() > transmitsWhileDead);               // transmitting again
    CHECK(d.severity() != MockShiftDriver::Severity::Error);     // and the error is cleared
}

// **THE INVARIANT THE STREAMING RING RESTS ON.** The ring never materialises the big encoded frame:
// the DMA loops a few small INTERNAL buffers, and the CPU encodes each slice straight into the buffer
// the DMA is about to read. That is only sound if encoding in slices produces EXACTLY the bytes the
// whole-frame encode would have produced — otherwise the wire sees a different frame depending on how
// it happened to be chunked, which is the class of bug that is invisible on a sparse effect and
// catastrophic on a dense one.
//
// Note the latch pad: only the LAST slice closes the frame (closeFrame), because the pad is what makes
// every strand idle LOW into the WS2812 reset. A pad emitted mid-frame would reset the strand halfway.
TEST_CASE("streaming ring: a sliced encode is byte-identical to the whole-frame encode") {
    mm::Correction corr;
    MockShiftDriver whole;
    mm::Buffer srcWhole;
    wire(whole, srcWhole, corr, 16 * 32, "1,2", /*shiftOn=*/true, /*latch=*/3);
    REQUIRE(whole.maxLaneLights() == 32);

    // Fill the source with a dense, varied pattern — a sparse/zero buffer would hide a slicing bug.
    for (nrOfLightsType i = 0; i < 16 * 32; i++) {
        uint8_t* px = srcWhole.data() + i * 3;
        px[0] = static_cast<uint8_t>(i * 7 + 1);
        px[1] = static_cast<uint8_t>(i * 13 + 2);
        px[2] = static_cast<uint8_t>(i * 29 + 3);
    }

    const uint8_t outCh = 3;
    const size_t bytes = whole.frameBytes();
    std::vector<uint8_t> refBuf(bytes, 0), sliceBuf(bytes, 0);

    // Reference: the whole frame in one call (what the driver does today).
    whole.encodeSliceForTest<uint8_t>(outCh, refBuf.data(), 0, 0, /*closeFrame=*/true);

    // The ring's way: the same frame in slices of 8 rows, each written at its own offset. Only the
    // final slice closes the frame.
    const nrOfLightsType rows = whole.maxLaneLights();
    const nrOfLightsType per = 8;
    // Bytes one row emits: channels x 8 bits x 3 slots x outputsPerPin, at 1 byte per slot (8-bit bus).
    const size_t rowBytes = static_cast<size_t>(outCh) * 8 * 3 * 8;
    for (nrOfLightsType first = 0; first < rows; first += per) {
        const bool last = (first + per >= rows);
        whole.encodeSliceForTest<uint8_t>(outCh, sliceBuf.data() + first * rowBytes,
                                          first, per, /*closeFrame=*/last);
    }

    CHECK(std::memcmp(refBuf.data(), sliceBuf.data(), bytes) == 0);
}

// **The pin list handed to the peripheral must be exactly as long as the count that goes with it.**
// The BUS is 8 or 16 bits wide whatever the board drives, so a 3-pin board still hands the peripheral
// an 8-entry list: 3 data lanes, then the spares parked on the clock pin (a real GPIO the peripheral
// already drives, so the lane is inert). This is what lets a board name only the pins it uses.
//
// The bug this pins: busPinList() used to shortcut `if (!shiftMode()) return laneList_` — the RAW,
// unpadded list — while busPinCount() reported the rounded width. With fewer pins than the bus is
// wide, the platform would then read past the end of laneList_. Direct mode never hit it only because
// the validation rejected any count but 8 or 16; allowing any count is what exposes it.
TEST_CASE("bus pin list is padded to the full bus width, in both modes") {
    mm::Buffer src;
    mm::Correction corr;

    SUBCASE("direct mode: 3 pins → 8 lanes, 5 parked on the clock pin") {
        MockShiftDriver d;
        wire(d, src, corr, 64, "1,2,4", /*shiftOn=*/false, /*latch=*/-1);

        REQUIRE(d.busPinCountForTest() == 8);        // the BUS width, not the pin count
        const uint16_t* list = d.busPinListForTest();
        CHECK(list[0] == 1);
        CHECK(list[1] == 2);
        CHECK(list[2] == 4);
        for (uint8_t i = 3; i < 8; i++) CHECK(list[i] == 99);   // parked on clockPinForBus()
    }

    SUBCASE("shift mode: 2 pins + latch → 8 lanes, the rest parked") {
        MockShiftDriver d;
        wire(d, src, corr, 64, "1,2", /*shiftOn=*/true, /*latch=*/7);

        REQUIRE(d.busPinCountForTest() == 8);
        const uint16_t* list = d.busPinListForTest();
        CHECK(list[0] == 1);
        CHECK(list[1] == 2);
        CHECK(list[2] == 7);                                    // the latch takes bus bit physPins_
        for (uint8_t i = 3; i < 8; i++) CHECK(list[i] == 99);
    }

    // (The 16-bit bus is exercised in unit_I80LedDriver, on a driver whose chip actually HAS 16 lanes;
    // this mock declares 8, so a 9th pin is correctly clamped away rather than widening the bus.)
}

// **The shift-mode loopback frame must reserve the trailing latch word it unconditionally writes.**
//
// encodeLoopbackFrameShift emits `lights` rows and then ALWAYS calls encodeWs2812ShiftLatchPad to
// close the frame — one extra Slot past the last row. The buffer was sized for the rows only, so that
// final word landed one-to-two bytes past the end of the heap block: a real overrun on every shift-mode
// loopback, silent on a forgiving allocator and a crash on an unlucky one. (Direct mode writes no pad,
// so it is unaffected — and that asymmetry is why sizing them the same was wrong.)
//
// The bug is a heap write, so the assertion that really catches it is the sanitizer, not a CHECK: run
// this suite under ASan and the pre-fix code trips heap-buffer-overflow here. What the CHECKs below can
// pin on their own is that the loopback ran to completion and left the driver healthy rather than
// tripping the out-of-memory path.
TEST_CASE("shift register: the loopback frame has room for its closing latch word") {
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 16 * 8, "1,2", /*shiftOn=*/true, /*latch=*/3);
    REQUIRE(d.laneCount() == 16);          // 2 pins x 8 outputs

    d.loopbackTest = true;
    d.loopbackStrand = 0;
    d.applyState();                        // arms the self-test
    d.tick();                              // builds + "transmits" the test frame (mock bus)

    // It must not have fallen into the out-of-memory branch, and must still be a live driver.
    CHECK(d.severity() != MockShiftDriver::Severity::Error);
    CHECK(d.laneCount() == 16);
}
