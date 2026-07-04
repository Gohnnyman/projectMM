// @module IrModule
// @also Scheduler

// Pins IrModule's action path: its buttons adjust another module's control through the shared
// Scheduler::setControl primitive, clamped to the control's own bounds. A fake "Drivers" module
// (a Knob with brightness + palette controls) stands in for the real one, so the test needs no
// light-domain modules — it proves the generic relative-adjust behaviour in isolation. The IR
// *reception* side is a platform stub (irRead returns false), so this focuses on the action
// path, which is the working, testable part of this cut.

#include "doctest.h"
#include "core/IrModule.h"
#include "core/Scheduler.h"
#include "core/MoonModule.h"

#include <cstring>

using namespace mm;

namespace {

// Stands in for Drivers: a brightness Uint8 (0–255) and a palette Select (0–3). Named "Drivers"
// so IrModule's kActions ("Drivers"/"brightness", "Drivers"/"palette") resolve to it.
struct FakeDrivers : public MoonModule {
    uint8_t brightness = 100;
    uint8_t palette = 1;
    void onBuildControls() override {
        controls_.addUint8("brightness", brightness, 0, 255);
        // A Select's max is (optionCount - 1); addSelect binds min 0 / max count-1.
        static const char* kPalettes[] = {"A", "B", "C", "D"};
        controls_.addSelect("palette", palette, kPalettes, 4);
    }
};

// Build Scheduler + FakeDrivers + IrModule, run setup so Scheduler::instance() is live and
// controls are bound. Caller owns teardown via the scheduler (modules are heap-allocated).
struct Rig {
    Scheduler scheduler;
    FakeDrivers* drivers = new FakeDrivers();
    IrModule* ir = new IrModule();
    Rig() {
        drivers->setName("Drivers");
        ir->setName("Ir");
        scheduler.addModule(drivers);
        scheduler.addModule(ir);
        scheduler.setup();   // binds controls + sets Scheduler::instance()
    }
    ~Rig() { scheduler.teardown(); }
    // Learn `code` to the action at `learnIndex` (1-based: 1=brightness up, 2=down, 3=palette
    // next, 4=prev), then that code drives the action on later inject. Mirrors the on-device flow:
    // arm the learn select → the next received code binds → subsequent codes fire the action.
    void learn(int learnIndex, uint32_t code) {
        char v[24]; std::snprintf(v, sizeof(v), "{\"value\":%d}", learnIndex);
        Scheduler::instance()->setControl("Ir", "learn", v);
        ir->injectCodeForTest(code);   // binds + disarms
    }
    void fire(uint32_t code) { ir->injectCodeForTest(code); }   // a received code drives its action
};

}  // namespace

TEST_CASE("IrModule: a learned code adjusts Drivers brightness by the step") {
    Rig r;
    r.drivers->brightness = 100;
    r.learn(1, 0xB1);   // bind code 0xB1 → brightness up
    r.learn(2, 0xB2);   // bind code 0xB2 → brightness down
    r.fire(0xB1);
    CHECK(r.drivers->brightness == 116);   // +16
    r.fire(0xB2);
    r.fire(0xB2);
    CHECK(r.drivers->brightness == 84);    // 116 - 32
}

TEST_CASE("IrModule: firing a learned code reports what it changed via status") {
    Rig r;
    r.drivers->brightness = 100;
    // Before setup drives anything, the pin is unset in the rig → readiness warns to set it.
    CHECK(std::strstr(r.ir->status(), "set pin") != nullptr);
    r.learn(1, 0xB1);
    r.fire(0xB1);
    // The action acknowledges the change it made, naming the target control + new value.
    CHECK(std::strcmp(r.ir->status(), "Drivers.brightness → 116") == 0);
}

TEST_CASE("IrModule: pin state drives readiness status") {
    Rig r;
    CHECK(std::strstr(r.ir->status(), "set pin") != nullptr);   // unset pin → warns to set it
    // Set a valid pin through the real path; onBuildState() re-reports readiness.
    Scheduler::instance()->setControl("Ir", "pin", "{\"value\":5}");
    CHECK(std::strcmp(r.ir->status(), "ready") == 0);
    // Back to unset → warns again.
    Scheduler::instance()->setControl("Ir", "pin", "{\"value\":-1}");
    CHECK(std::strstr(r.ir->status(), "set pin") != nullptr);
}

TEST_CASE("IrModule: a learned brightness code clamps at 0 and 255") {
    Rig r;
    r.learn(1, 0xB1);   // brightness up
    r.learn(2, 0xB2);   // brightness down
    r.drivers->brightness = 250;
    r.fire(0xB1);
    CHECK(r.drivers->brightness == 255);   // 250+16 clamps to 255, not wrap
    r.drivers->brightness = 8;
    r.fire(0xB2);
    CHECK(r.drivers->brightness == 0);     // 8-16 clamps to 0, not underflow
}

TEST_CASE("IrModule: learned palette codes step the Select and clamp at the ends") {
    Rig r;
    r.learn(3, 0xB3);   // palette next
    r.learn(4, 0xB4);   // palette prev
    r.drivers->palette = 1;
    r.fire(0xB3);
    CHECK(r.drivers->palette == 2);
    r.fire(0xB4);
    r.fire(0xB4);
    CHECK(r.drivers->palette == 0);        // 2 → 1 → 0
    r.fire(0xB4);
    CHECK(r.drivers->palette == 0);        // clamps at the low end (options 0..3)
    r.drivers->palette = 3;
    r.fire(0xB3);
    CHECK(r.drivers->palette == 3);        // clamps at the high end
}

TEST_CASE("IrModule: learn binds a code to an action, then that code drives it") {
    Rig r;
    r.drivers->brightness = 100;
    // Arm learning for "brightness up" (learn select index 1 = first action).
    Scheduler::instance()->setControl("Ir", "learn", "{\"value\":1}");
    // A code arrives → it binds to "brightness up" and learning disarms; brightness unchanged yet.
    r.ir->injectCodeForTest(0xFA057F80);
    CHECK(r.drivers->brightness == 100);
    CHECK(std::strstr(r.ir->status(), "learned brightness up") != nullptr);
    // The SAME code now drives the bound action.
    r.ir->injectCodeForTest(0xFA057F80);
    CHECK(r.drivers->brightness == 116);
    r.ir->injectCodeForTest(0xFA057F80);
    CHECK(r.drivers->brightness == 132);
}

TEST_CASE("IrModule: an unlearned code is reported as unassigned, drives nothing") {
    Rig r;
    r.drivers->brightness = 100;
    r.ir->injectCodeForTest(0xDEADBEEF);   // never learned
    CHECK(r.drivers->brightness == 100);
    CHECK(std::strstr(r.ir->status(), "unassigned") != nullptr);
    CHECK(r.ir->latestCode() == 0xDEADBEEF);
}

TEST_CASE("IrModule: two codes bind to two actions independently") {
    Rig r;
    r.drivers->brightness = 100;
    r.drivers->palette = 1;
    r.learn(1, 0x11111111);   // brightness up
    r.learn(3, 0x22222222);   // palette next
    // Each code drives only its own action.
    r.fire(0x11111111);
    CHECK(r.drivers->brightness == 116);
    CHECK(r.drivers->palette == 1);        // unchanged
    r.fire(0x22222222);
    CHECK(r.drivers->palette == 2);
    CHECK(r.drivers->brightness == 116);   // unchanged
}

TEST_CASE("IrModule: an unassigned code is a no-op, not a crash") {
    Rig r;
    const uint8_t before = r.drivers->brightness;
    r.fire(0xABCDEF01);                    // no learned binding → nothing happens
    CHECK(r.drivers->brightness == before);
    // The code is still recorded (for the read-out) even though it drove no action.
    CHECK(r.ir->latestCode() == 0xABCDEF01);
}

TEST_CASE("IrModule: a learned code whose target module is gone is a no-op, reported") {
    Rig r;
    const uint8_t before = r.drivers->brightness;
    r.learn(1, 0xC0DE);                    // bind 0xC0DE → brightness up (targets "Drivers")
    // Take the target out of reach: rename it so firstByName("Drivers") returns null.
    r.drivers->setName("NotDrivers");
    r.fire(0xC0DE);                        // the action fires but its module is missing
    CHECK(r.drivers->brightness == before);            // nothing changed
    CHECK(std::strstr(r.ir->status(), "no Drivers module") != nullptr);  // reported, not crashed
}
