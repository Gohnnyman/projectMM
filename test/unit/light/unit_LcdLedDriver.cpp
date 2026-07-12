// @module LcdLedDriver
// @also Drivers, Correction

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/LcdLedDriver.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // shared conditional-control helpers

#include <cstring>

// Host-side half of the LCD driver: lane slicing (the shared PinList
// semantics), the frame-byte arithmetic (latch pad, 64-byte alignment, RGBW
// growth), and the parse-error/recovery shape. The hardware half (bus init,
// DMA transmit) is inert on the host — desktop stubs return false/nullptr —
// and is proven on the S3.

namespace {

void wire(mm::LcdLedDriver& d, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    // Pins default to UNSET now (the "default only when it cannot do harm" rule —
    // a user solders the strand to its own GPIOs), so a fresh driver idles until
    // configured. These slicing/frame tests exercise the lane logic, not the
    // default value, so the helper supplies the bench 8-pin set unless a case set
    // its own pins first (the bad-pin / partial-bus cases do).
    if (d.pins[0] == '\0') std::strcpy(d.pins, "1,2,4,5,6,7,8,9");
    // allocate succeeds exactly when lights > 0 (the zero-grid case wires an
    // empty buffer on purpose); a masked alloc failure would fail cases downstream.
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);   // 3 out-channels
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

// frameBytes = maxLaneLights × outCh × 24 + 800 latch pad + 64 clock-tolerance
// slack, rounded up to 64 (mirrors ParallelLedDriver::frameBytesFor).
size_t expectFrame(mm::nrOfLightsType maxLights, uint8_t outCh) {
    if (maxLights == 0) return 0;
    const size_t raw = static_cast<size_t>(maxLights) * outCh * 24 + 800 + 64;
    return (raw + 63) & ~static_cast<size_t>(63);
}

} // namespace

// Explicit counts slice the buffer consecutively; the frame is sized by the
// LONGEST lane. The bus always has all 8 lanes — unused strands take the
// 0-light remainder and idle LOW.
TEST_CASE("LcdLedDriver slices lanes and sizes the frame by the longest") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,20,20");   // lanes 3..7 share the remainder: 0
    wire(d, src, corr, 90);

    REQUIRE(d.laneCount() == 8);
    CHECK(d.laneLightCount(0) == 50);
    CHECK(d.laneLightCount(1) == 20);
    CHECK(d.laneLightCount(2) == 20);
    CHECK(d.laneLightCount(3) == 0);
    CHECK(d.laneLightCount(7) == 0);
    CHECK(d.laneStart(0) == 0);
    CHECK(d.laneStart(1) == 50);
    CHECK(d.laneStart(2) == 70);
    CHECK(d.maxLaneLights() == 50);
    CHECK(d.frameBytes() == expectFrame(50, 3));
}

// Empty ledsPerPin splits evenly — same PinList semantics the RMT driver uses.
TEST_CASE("LcdLedDriver even split over the default 8 lanes") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 256);   // default pins: 8 lanes

    REQUIRE(d.laneCount() == 8);
    CHECK(d.laneLightCount(0) == 32);
    CHECK(d.laneLightCount(7) == 32);
    CHECK(d.maxLaneLights() == 32);
    CHECK(d.frameBytes() == expectFrame(32, 3));
}

// An RGB→RGBW preset toggle grows the frame (32 vs 24 slot bytes per light).
TEST_CASE("LcdLedDriver frame grows on RGBW preset") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,50");   // lanes 2..7 idle
    wire(d, src, corr, 100);
    CHECK(d.frameBytes() == expectFrame(50, 3));

    // The driver owns its Correction, so mutate that copy (not the external one).
    mm::test::rebuildFromPreset(d.correctionForTest(), 255, mm::test::PresetOrder::GRBW);
    d.onCorrectionChanged();
    CHECK(d.frameBytes() == expectFrame(50, 4));
}

// A bad pin list idles the driver with the parse literal in the status; fixing it recovers.
TEST_CASE("LcdLedDriver bad pins → status error → recovery") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,nope");
    wire(d, src, corr, 64);

    CHECK(d.laneCount() == 0);
    CHECK(d.frameBytes() == 0);
    CHECK(d.status() != nullptr);

    std::strcpy(d.pins, "1,2,4,5,6,7,8,9");
    d.applyState();
    CHECK(d.laneCount() == 8);
    CHECK(d.status() == nullptr);
}

// Pins now default UNSET (the "default only when it cannot do harm" rule — the
// strand is user-soldered). A fresh, unconfigured driver idles, never grabbing
// the 8 data GPIOs on its own. (wire() back-fills empty pins for the slicing
// cases, so this one wires the buffer directly to keep pins empty.)
TEST_CASE("LcdLedDriver with the empty default pins idles cleanly") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    REQUIRE(d.pins[0] == '\0');           // the empty default, not a bench guess
    REQUIRE(src.allocate(64, 3));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();

    CHECK(d.laneCount() == 0);            // no lanes claimed
    CHECK(d.frameBytes() == 0);
    CHECK(d.status() != nullptr);         // "set pins" surfaced, not silent
    d.tick();                             // must be a no-op, not a crash
}

// IDF's i80 bus rejects partial pin sets, so the driver does too — fewer than
// 8 pins is a config error, not a narrower bus.
TEST_CASE("LcdLedDriver requires exactly 8 pins") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,2,4");
    wire(d, src, corr, 64);

    CHECK(d.laneCount() == 0);
    CHECK(d.frameBytes() == 0);
    REQUIRE(d.status() != nullptr);
    CHECK(std::strcmp(d.status(), "LCD bus needs exactly 8 pins") == 0);
}

// A data lane on the same GPIO as the WR (clockPin) or DC pin is rejected loud:
// the i80 matrix would route two output signals to one pin and that lane would
// emit the clock/DC waveform instead of pixel data (silent strip corruption).
// IDF doesn't catch this, so the driver must. clockPin/dcPin default to 10/11.
TEST_CASE("LcdLedDriver rejects a data pin that collides with clockPin/dcPin") {
    mm::Buffer src;
    mm::Correction corr;
    {   // lane on GPIO 10 == default clockPin
        mm::LcdLedDriver d;
        std::strcpy(d.pins, "18,5,6,7,8,9,10,11");   // 10 == clockPin, 11 == dcPin
        wire(d, src, corr, 64);
        CHECK(d.laneCount() == 0);                    // idled, not built
        REQUIRE(d.status() != nullptr);
        CHECK(std::strcmp(d.status(), "LED pin collides with clockPin (WR)") == 0);
    }
    {   // move clock/dc clear of the data set → builds cleanly (the fix on the bench)
        mm::LcdLedDriver d;
        d.clockPin = 12;
        d.dcPin = 13;
        std::strcpy(d.pins, "18,5,6,7,8,9,10,11");
        wire(d, src, corr, 64);
        CHECK(d.laneCount() == 8);
        CHECK(d.status() == nullptr);
    }
}

// A 0×0×0 grid is a clean idle: zero counts, zero frame (no pad for an empty frame), no crash.
TEST_CASE("LcdLedDriver tolerates a zero-light buffer") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 0);

    CHECK(d.laneCount() == 8);       // pins parse fine
    CHECK(d.maxLaneLights() == 0);
    CHECK(d.frameBytes() == 0);
    d.tick();                        // must be a no-op, not a crash
    CHECK(true);
}

// setup/release cycles leave no residue (status clean, ASAN-checked heap).
TEST_CASE("LcdLedDriver setup/release is repeatable") {
    mm::LcdLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    src.allocate(64, 3);
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    std::strcpy(d.pins, "1,2,4,5,6,7,8,9");   // pins now default UNSET
    d.defineControls();
    for (int cycle = 0; cycle < 4; cycle++) {
        d.setup();
        d.setSourceBuffer(&src);
        d.correctionForTest() = corr;
        d.applyState();
        REQUIRE(d.laneCount() == 8);
        d.release();
        CHECK(d.status() == nullptr);
    }
}

// loopbackRxPin is bound always, visible only while loopbackTest is on.
TEST_CASE("LcdLedDriver loopbackRxPin tracks the loopbackTest toggle") {
    mm::LcdLedDriver d;
    d.defineControls();
    bool found = false;
    for (uint8_t i = 0; i < d.controls().count(); i++) {
        if (std::strcmp(d.controls()[i].name, "loopbackRxPin") == 0) {
            found = true;
            CHECK(d.controls()[i].hidden == true);   // test mode off by default
        }
    }
    CHECK(found);
}

// loopbackTxPin (optional lane-0 TX override) is bound always, hidden until the
// test is on — same conditional-control contract as loopbackRxPin. The override's
// lane-0 substitution is hardware-only (lcdLanes==0 on desktop); the visibility
// contract is host-testable here via the shared helper (toggles loopbackTest both
// ways and asserts the control stays bound while flipping visibility).
TEST_CASE("LcdLedDriver loopbackTxPin tracks the loopbackTest toggle") {
    mm::LcdLedDriver d;
    d.defineControls();
    auto setTest = [&](bool on) {
        mm::test::setControlValue<bool>(d, "loopbackTest", on);
    };
    mm::test::checkConditionalControl(d, "loopbackTxPin", setTest, /*visibleWhenTrue=*/true);
}
