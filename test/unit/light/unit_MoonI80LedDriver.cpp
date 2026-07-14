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
//   - its own bus-pin validation, and the fact that it needs FEWER pins than the sibling: no DC at
//     all, and WR only under the expander (owning the GPIO matrix is what buys that).
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

// The i80 BUS is 8 or 16 bits wide whatever the pin count, so the base rounds it up (kPowerOfTwoBus)
// and parks the lanes the board does not use. And the loopback cannot build a 1-lane private bus, so
// its test frame is encoded at the full operational width.
TEST_CASE("MoonI80LedDriver keeps the i80 bus rules: power-of-two bus, full-width loopback") {
    CHECK(mm::MoonI80LedDriver::kPowerOfTwoBus);
    CHECK(mm::MoonI80LedDriver::kLoopbackFullWidth);
}

// **The bus control pins are a '595 cost, not an i80 cost — and owning the DMA is what proves it.**
// DC exists so an LCD panel can separate command bytes from data bytes; a WS2812 strand has no such
// concept, and the peripheral holds DC at a constant level. WR is the pixel clock, which only a shift
// register consumes (as SRCLK) — WS2812 is self-clocked, so a strand ignores it. esp_lcd mandates a
// valid GPIO for BOTH regardless (`wr_gpio_num >= 0 && dc_gpio_num >= 0`), which is why the sibling
// still spends two pins on them. This backend routes its own GPIO matrix, so it routes neither in
// direct mode: there is no dcPin at all, and clockPin reaches a pad only under the expander.
//
// The observable consequence, and what this case pins: in DIRECT mode `clockPin` may freely name a
// GPIO that a strand also uses, because the signal never leaves the peripheral. Rejecting that would
// forbid a working config to protect a signal nobody reads.
TEST_CASE("MoonI80LedDriver direct mode: clockPin is unrouted, so it cannot collide") {
    mm::MoonI80LedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,2,4,5,6,7,8,10");   // pin 10 IS the default clockPin (WR) — fine here
    wire(d, src, corr, 8 * 16);
    CHECK(d.severity() != mm::MoonI80LedDriver::Severity::Error);
    CHECK(d.laneCount() == 8);   // it drives all eight strands
}

// Under the expander WR IS routed (it clocks the '595s), so now it can collide — and a data lane
// sharing it is silent corruption: the matrix drives both signals onto the one pad and that strand
// emits the shift clock instead of pixel data.
TEST_CASE("MoonI80LedDriver shift mode: a data pin on clockPin (WR) is caught") {
    mm::MoonI80LedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.shiftRegister = true;
    d.latchPin = 12;
    std::strcpy(d.pins, "1,2,10");   // pin 10 IS clockPin, and here WR is a real pad
    wire(d, src, corr, 8 * 16);
    CHECK(d.severity() != mm::MoonI80LedDriver::Severity::Status);   // it complains
}

// The '595 latch rides a DATA lane (the peripheral gives only one clock output, and WR is already the
// shift clock), so it must not land on WR — the latch would ride the shift clock itself and nothing
// would ever latch, which looks like a dead strip rather than a config error.
TEST_CASE("MoonI80LedDriver rejects a latchPin on WR") {
    mm::MoonI80LedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.shiftRegister = true;
    d.latchPin = d.clockPin;
    wire(d, src, corr, 8 * 16);
    CHECK(d.severity() == mm::MoonI80LedDriver::Severity::Error);
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
