// @module RmtLedDriver
// @also Drivers, Correction

#include "doctest.h"
#include "light/drivers/RmtLedDriver.h"
#include "light/drivers/Correction.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // shared conditional-control helpers

#include <cstring>  // std::strcpy (writing the pins text control directly)

// These tests pin the symbol-buffer LIFECYCLE — the exact class of bug that
// reached hardware: a review fix made deinit() free symbols_, and because
// reinit() calls deinit(), every rebuild freed the buffer loop() needs, so the
// driver silently stopped transmitting. None of that touches the RMT peripheral
// (ESP32-only); it's pure host-testable buffer ownership, which is why a unit
// test is the right guard. The symbolBuffer()/symbolCapacity() accessors are
// test-only (mirror ArtNet's correctedBuffer()).

namespace {

// Wire a driver up to a source buffer + correction the way the Drivers container
// does, then run onBuildState (the sizing hook). Returns nothing; the caller
// inspects the driver.
void wire(mm::RmtLedDriver& d, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights = 64) {
    REQUIRE(src.allocate(lights, 3));   // a masked alloc failure would fail cases downstream
    corr.rebuild(255, mm::LightPreset::GRB);   // 3 out-channels
    d.onBuildControls();
    d.setSourceBuffer(&src);
    d.setCorrection(&corr);
    d.applyState();
}

} // namespace

TEST_CASE("RmtLedDriver sizes the symbol buffer in onBuildState") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);

    // 64 lights × 3 channels × 8 bits = 1536 symbols.
    REQUIRE(d.symbolBuffer() != nullptr);
    CHECK(d.symbolCapacity() >= static_cast<size_t>(64) * 3 * 8);
}

TEST_CASE("RmtLedDriver keeps the symbol buffer across a rebuild (reinit must not free it)") {
    // The regression: onBuildState() does resizeSymbols() THEN reinit(), and a
    // bad reinit()->deinit() freed symbols_ right after it was allocated, so the
    // buffer was null by the time loop() ran. A second onBuildState (what a pins
    // change / topology rebuild triggers) must leave the buffer present.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.symbolBuffer() != nullptr);

    d.applyState();   // simulate a rebuild (the path that runs reinit())
    CHECK(d.symbolBuffer() != nullptr);   // would be null with the deinit()-frees bug
    CHECK(d.symbolCapacity() >= static_cast<size_t>(64) * 3 * 8);
}

TEST_CASE("RmtLedDriver keeps the symbol buffer across a pins change") {
    // Same regression class as above, multi-pin flavour: editing the pins list
    // triggers a rebuild that re-parses and re-inits N channels — none of which
    // may free the symbol buffer loop() encodes into.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.symbolBuffer() != nullptr);

    std::strcpy(d.pins, "18,17");
    d.applyState();
    CHECK(d.symbolBuffer() != nullptr);
    CHECK(d.pinCount() == 2);
}

TEST_CASE("RmtLedDriver grows the symbol buffer when the grid grows") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 16);
    const size_t cap16 = d.symbolCapacity();
    CHECK(cap16 >= static_cast<size_t>(16) * 3 * 8);

    // Grow the source to 256 lights and rebuild: capacity must grow to fit.
    src.allocate(256, 3);
    d.setSourceBuffer(&src);
    d.applyState();
    CHECK(d.symbolBuffer() != nullptr);
    CHECK(d.symbolCapacity() >= static_cast<size_t>(256) * 3 * 8);
}

TEST_CASE("RmtLedDriver releases the symbol buffer on teardown") {
    // The leak CodeRabbit flagged: teardown must free the buffer (and only
    // teardown — not deinit, which reinit calls).
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.symbolBuffer() != nullptr);

    d.teardown();
    CHECK(d.symbolBuffer() == nullptr);
    CHECK(d.symbolCapacity() == 0);
}

TEST_CASE("RmtLedDriver: disabling releases the resource, re-enabling re-acquires (applyState)") {
    // Core's applyState() routes an effectively-enabled module to onBuildState() (acquire) and a
    // disabled one to teardown() (release), so setEnabled(false) + the post-toggle buildState() sweep
    // frees the peripheral + buffer (pins reusable). Observable via the symbol buffer: present when
    // enabled, gone after the disabled sweep, rebuilt on the re-enabled sweep.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.symbolBuffer() != nullptr);   // enabled by default → resource held

    // The Scheduler runs a whole-tree buildState() AFTER the enabled-toggle (Scheduler::setControl),
    // which calls applyState() on every module. A just-disabled one routes to teardown → released,
    // and must STAY released on subsequent sweeps (else a disabled RMT driver re-grabs a GPIO an
    // enabled Parlio driver now owns).
    d.setEnabled(false);
    d.applyState();                         // disabled → teardown
    CHECK(d.symbolBuffer() == nullptr);     // buffer freed (RAM back; pins released)
    CHECK(d.symbolCapacity() == 0);

    d.applyState();                         // a later unrelated sweep — STILL released
    CHECK(d.symbolBuffer() == nullptr);

    d.setEnabled(true);
    d.applyState();                         // enabled → onBuildState re-acquires
    CHECK(d.symbolBuffer() != nullptr);     // resource re-acquired on re-enable
}

TEST_CASE("RmtLedDriver: a DISABLED driver does not acquire through the boot sweep") {
    // Core's applyState() (the boot Phase-4 sweep + every buildState) calls onBuildState() only when
    // effectively-enabled, else teardown(). A driver persisted DISABLED but sharing a GPIO with an
    // enabled sibling must not grab the peripheral/pins — else it steals the pin and the enabled
    // sibling's output stays dead. This reproduces the hardware symptom: a disabled RmtLed on GPIO 20
    // blanking an enabled ParlioLed on the same pin.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    REQUIRE(src.allocate(64, 3));
    corr.rebuild(255, mm::LightPreset::GRB);
    d.onBuildControls();
    d.setEnabled(false);          // persisted-disabled at boot
    d.setSourceBuffer(&src);
    d.setCorrection(&corr);

    d.setup();                    // Phase 3: pure wiring, no acquire
    d.applyState();               // Phase 4: disabled → routes to teardown, no acquire
    CHECK(d.symbolBuffer() == nullptr);

    d.setEnabled(true);           // now enable → acquire on the next sweep
    d.applyState();
    CHECK(d.symbolBuffer() != nullptr);
}

// MoonModule contract: teardown reverses setup, so setup→teardown→setup→teardown
// cycles leave no residue — no leaked heap (ASAN in the test runner catches that),
// no stuck state. After each teardown the driver must look untouched: no symbol
// buffer, no status. Run several cycles to surface any accumulation.
TEST_CASE("RmtLedDriver setup/teardown is repeatable with no residual state") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    src.allocate(64, 3);
    corr.rebuild(255, mm::LightPreset::GRB);
    d.onBuildControls();

    for (int cycle = 0; cycle < 4; cycle++) {
        d.setup();                       // (re)init the channel
        d.setSourceBuffer(&src);         // resizeSymbols allocates the buffer
        d.setCorrection(&corr);
        d.applyState();                // size buffer + reinit, as the Scheduler does
        REQUIRE(d.symbolBuffer() != nullptr);

        d.teardown();                    // must fully reverse the above
        CHECK(d.symbolBuffer() == nullptr);   // buffer freed (ASAN: no leak across cycles)
        CHECK(d.symbolCapacity() == 0);
        CHECK(d.status() == nullptr);         // no lingering status string
    }
}

// Conditional control: loopbackRxPin is visible only while loopbackTest is on,
// hidden otherwise — but always bound (so a saved rxPin loads regardless). Same
// add-then-setHidden pattern as NetworkModule (architecture.md § Conditional
// controls). This pins the exact behavior that, with the old UI, showed the pin
// at the wrong times; a regression in the C++ flag now fails here.
TEST_CASE("RmtLedDriver loopbackRxPin tracks the loopbackTest toggle") {
    mm::RmtLedDriver d;
    d.onBuildControls();
    auto setTest = [&](bool on) {
        mm::test::setControlValue<bool>(d, "loopbackTest", on);
    };
    mm::test::checkConditionalControl(d, "loopbackRxPin", setTest, /*visibleWhenTrue=*/true);
}

// loopbackTxPin is the optional TX override (transmit on it instead of pins[0]
// during the self-test). Like loopbackRxPin it's a conditional control: always
// bound (so a saved override loads), shown only while loopbackTest is on. The
// override's effect on the transmitted pin is hardware-only (rmtTxChannels==0 on
// desktop), but the conditional-visibility contract is host-testable here.
TEST_CASE("RmtLedDriver loopbackTxPin tracks the loopbackTest toggle") {
    mm::RmtLedDriver d;
    d.onBuildControls();
    auto setTest = [&](bool on) {
        mm::test::setControlValue<bool>(d, "loopbackTest", on);
    };
    mm::test::checkConditionalControl(d, "loopbackTxPin", setTest, /*visibleWhenTrue=*/true);
}

// Editing `pins` while the loopback test is ON must refresh the parsed config
// before the self-test runs — onUpdate fires before the buildState sweep re-parses,
// so without the in-branch parseConfig() the test would transmit on the OLD pin and
// show a verdict for it. Mirrors the fix in ParallelLedDriver; this pins the RMT
// sibling that the dedup left behind. Host-observable via pinCount(): the refresh
// re-parses to the new pin set even though the platform loopback itself is inert.
TEST_CASE("RmtLedDriver loopback re-parses pins before testing (no stale-pin verdict)") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    src.allocate(64, 3);
    corr.rebuild(255, mm::LightPreset::GRB);
    std::strcpy(d.pins, "18");
    d.onBuildControls();
    d.setSourceBuffer(&src);
    d.setCorrection(&corr);
    d.applyState();
    REQUIRE(d.pinCount() == 1);

    // Turn the test on, then edit pins to a 3-pin set and fire the pin update the
    // way the control framework does (write the buffer, then onUpdate). The config
    // must already reflect 3 pins by the time the self-test reads pinList_.
    mm::test::setControlValue<bool>(d, "loopbackTest", true);
    std::strcpy(d.pins, "18,17,16");
    d.onUpdate("pins");
    CHECK(d.pinCount() == 3);   // refreshed before the test, not the stale 1
}
