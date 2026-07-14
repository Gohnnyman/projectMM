// @module MoonI80LedDriver
// @also I80LedDriver, ParallelLedDriver

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/MoonI80LedDriver.h"
#include "light/layers/Buffer.h"

#include <cstring>

// MoonI80LedDriver is the SAME LCD_CAM output as I80LedDriver, on our own DMA code instead of
// esp_lcd (ADR-0014). It is a CRTP sibling of the same ParallelLedDriver base, so the base's whole
// body — lane slicing, frame sizing, the fused encode, the async double-buffer, the shift-register
// expander, the dead-frame guard — is ALREADY covered by the Mock-driver suites
// (unit_ParallelLedDriver_doublebuffer / _shiftregister) and by unit_I80LedDriver. Re-testing it
// here through a second concrete driver would assert the same base twice.
//
// So these cases pin only what is genuinely MoonI80's own:
//   - it satisfies the CRTP contract (it instantiates and configures at all);
//   - the constants that DIFFER from its sibling — chiefly that it is LCD_CAM-only;
//   - its own bus-pin validation (a data lane on WR/DC is silent strand corruption).
//
// The hardware half (our GDMA descriptor chain) is inert on the host — desktop stubs return
// false/nullptr and `lanesAvailable()` is 0 — and is proven on the S3 bench.

namespace {

void wire(mm::MoonI80LedDriver& d, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    if (d.pins[0] == '\0') std::strcpy(d.pins, "1,2,4,5,6,7,8,9");
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

// **The load-bearing difference from its sibling.** I80LedDriver runs on the i80 *bus* — LCD_CAM on
// the S3/P4 AND the I2S peripheral on the classic ESP32 (IDF's esp_lcd picks the backend). MoonI80
// programs LCD_CAM directly, so it must NOT claim the classic chip: `lanesAvailable()` reads
// `lcdLanes` alone, without the `+ i2sLanes` its sibling adds. Getting this wrong would offer the
// driver on a chip whose peripheral it cannot drive.
TEST_CASE("MoonI80LedDriver is LCD_CAM-only — it does not claim the classic ESP32's I2S i80") {
    CHECK(mm::MoonI80LedDriver::lanesAvailable() == mm::platform::lcdLanes);
    // The expander needs LCD_CAM, which is exactly where this driver runs — so the two agree.
    CHECK(mm::MoonI80LedDriver::kSupportsShiftRegister == (mm::platform::lcdLanes > 0));
}

// The i80 peripheral rejects a partial bus (it configures all 8 or all 16 data lines), so the base's
// exact-lane-count rule must be on — same as the sibling. And the loopback cannot build a 1-lane
// private bus, so its test frame is encoded at the full operational width.
TEST_CASE("MoonI80LedDriver keeps the i80 bus rules: exact lane count, full-width loopback") {
    CHECK(mm::MoonI80LedDriver::kExactLaneCount);
    CHECK(mm::MoonI80LedDriver::kLoopbackFullWidth);
}

// A data lane on WR or DC is SILENT corruption, not a clean failure: the GPIO matrix routes two
// signals to the one pin, and that strand emits the clock or DC waveform instead of pixel data. The
// driver must catch it rather than drive garbage.
TEST_CASE("MoonI80LedDriver rejects a data pin on the clock or DC pin") {
    mm::MoonI80LedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,2,4,5,6,7,8,10");   // pin 10 IS the default clockPin (WR)
    wire(d, src, corr, 8 * 16);
    CHECK(d.severity() != mm::MoonI80LedDriver::Severity::Status);   // it complains

    mm::MoonI80LedDriver d2;
    mm::Buffer src2;
    mm::Correction corr2;
    std::strcpy(d2.pins, "1,2,4,5,6,7,8,11");   // pin 11 IS the default dcPin
    wire(d2, src2, corr2, 8 * 16);
    CHECK(d2.severity() != mm::MoonI80LedDriver::Severity::Status);
}

// WR and DC on the same GPIO is fatal — the peripheral needs both, distinctly.
TEST_CASE("MoonI80LedDriver rejects clockPin == dcPin") {
    mm::MoonI80LedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.dcPin = d.clockPin;
    wire(d, src, corr, 8 * 16);
    CHECK(d.severity() == mm::MoonI80LedDriver::Severity::Error);
    CHECK(d.laneCount() == 0);   // idles rather than driving a bus it cannot build
}

// The '595 latch rides a DATA lane (the peripheral gives only one clock output, and WR is already
// the shift clock), so it must not collide with WR or DC either.
TEST_CASE("MoonI80LedDriver rejects a latchPin on WR or DC") {
    // On WR: the latch would ride the shift clock itself, so nothing would ever latch.
    mm::MoonI80LedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.shiftRegister = true;
    d.latchPin = d.clockPin;
    wire(d, src, corr, 8 * 16);
    CHECK(d.severity() == mm::MoonI80LedDriver::Severity::Error);

    // On DC: the symmetric branch. Both are fatal, and both must say so — a latch sharing a pin is a
    // '595 that never presents a byte, which looks like a dead strip rather than a config error.
    mm::MoonI80LedDriver d2;
    mm::Buffer src2;
    mm::Correction corr2;
    d2.shiftRegister = true;
    d2.latchPin = d2.dcPin;
    wire(d2, src2, corr2, 8 * 16);
    CHECK(d2.severity() == mm::MoonI80LedDriver::Severity::Error);
}

// Sanity: with a valid config the driver is a working CRTP sibling — it slices lanes and reports the
// lights it drives, exactly like its sibling. (The lane/frame ARITHMETIC itself is the base's, and is
// covered once, in unit_I80LedDriver and the Mock suites.)
TEST_CASE("MoonI80LedDriver drives a valid config like its sibling") {
    mm::MoonI80LedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 8 * 32);   // 8 lanes × 32 lights
    CHECK(d.laneCount() == 8);
}
