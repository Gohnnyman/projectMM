// @module Drivers

#include "doctest.h"
#include "light/drivers/Drivers.h"

// Regression: the UI's enable/disable toggle on a child driver (e.g. ArtNet,
// Preview) was a no-op — the driver kept running. Cause: Drivers::tick() called
// child(i)->tick() unconditionally, skipping the per-child `enabled` check that
// Layer::tick() does for effects and Layers::tick() does for child Layers.
// (The Scheduler only walks top-level modules, so it never sees these children.)
//
// These tests pin the gate so the regression can't return silently. A stub
// driver counts its loop calls; toggling `enabled` must flip whether the count
// advances.

namespace {

// Minimal DriverBase stub: counts loop calls. Ignores the source buffer it
// would normally consume — this test cares only about whether tick() runs.
class CountingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() override { loopCalls++; }
    int loopCalls = 0;
};

} // namespace

// A driver that captures the Correction* it's handed, so a test can read the resulting LUT.
class CorrectionCapturingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() override {}
    void setCorrection(const mm::Correction* c) override { correction = c; }
    const mm::Correction* correction = nullptr;
};

// The `on` control is master power: on=false scales the correction LUT to zero (output black) while
// PRESERVING the brightness value, so on=true restores the exact level. It rides the same cheap LUT
// rebuild as brightness (no pipeline realloc). This pins the shared power control IR/MQTT/WLED drive.
TEST_CASE("Drivers::on gates the correction LUT without clobbering brightness") {
    mm::Drivers drivers;
    CorrectionCapturingDriver drv;
    drivers.addChild(&drv);
    drivers.setup();                       // seeds correction_ from on(true)+brightness, hands it to drv
    REQUIRE(drv.correction != nullptr);

    drivers.brightness = 200;
    drivers.on = true;
    drivers.onControlChanged("brightness");        // rebuild LUT at full power
    CHECK(drivers.effectiveBrightness() == 200);
    CHECK(drv.correction->briLut[255] == 200);   // (255 * 200) / 255 == 200

    // Turn off → LUT scales to black, but the brightness value is untouched.
    drivers.on = false;
    drivers.onControlChanged("on");
    CHECK(drivers.brightness == 200);            // value preserved
    CHECK(drivers.effectiveBrightness() == 0);
    CHECK(drv.correction->briLut[255] == 0);     // output black

    // Turn back on → the exact level returns, no stored-value juggling.
    drivers.on = true;
    drivers.onControlChanged("on");
    CHECK(drv.correction->briLut[255] == 200);
}

// Disabled child drivers don't tick: toggling `enabled` flips whether that driver's tick() runs.
TEST_CASE("Drivers::tick() skips disabled child drivers") {
    mm::Drivers drivers;
    CountingDriver a, b;
    drivers.addChild(&a);
    drivers.addChild(&b);

    // Both enabled by default → both tick.
    drivers.tick();
    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 1);

    // Disable `a` → only `b` ticks.
    a.setEnabled(false);
    drivers.tick();
    CHECK(a.loopCalls == 1);  // unchanged
    CHECK(b.loopCalls == 2);

    // Disable `b` too → neither ticks.
    b.setEnabled(false);
    drivers.tick();
    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 2);

    // Re-enable `a` → only `a` ticks.
    a.setEnabled(true);
    drivers.tick();
    CHECK(a.loopCalls == 2);
    CHECK(b.loopCalls == 2);
}
