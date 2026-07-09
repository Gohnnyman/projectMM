// @module SystemModule

#include "doctest.h"
#include "core/SystemModule.h"

#include <cstring>

namespace {
// Stand-in wired-by-code child: counts the lifecycle callbacks a real fixed
// System child (Tasks, I2cScan) would use (setup to init, loop20ms/loop1s to
// poll + format). Pins that SystemModule's overridden setup()/loop1s() chain to
// base — without that, a child would never initialise or poll.
class CountingChild : public mm::MoonModule {
public:
    uint32_t setupCalls = 0, loop20msCalls = 0, loop1sCalls = 0;
    void setup() override { setupCalls++; }
    void loop20ms() override { loop20msCalls++; }
    void loop1s() override { loop1sCalls++; }
};
} // namespace

// On the desktop platform (MAC DE:AD:BE:EF:CA:FE), the auto-generated device name is "MM-CAFE" (last two MAC bytes).
TEST_CASE("SystemModule MAC-to-deviceName") {
    // Desktop platform returns MAC DE:AD:BE:EF:CA:FE
    // deviceName should be MM-CAFE (last two bytes)
    mm::SystemModule sys;
    sys.setup();
    CHECK(std::strcmp(sys.deviceName(), "MM-CAFE") == 0);
}

// deviceName is bound as a Text control to the MAC-derived default ("MM-CAFE" on the desktop platform).
TEST_CASE("SystemModule deviceName control") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();

    bool found = false;
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "deviceName") == 0) {
            CHECK(sys.controls()[i].type == mm::ControlType::Text);
            CHECK(std::strcmp(static_cast<char*>(sys.controls()[i].ptr), "MM-CAFE") == 0);
            found = true;
        }
    }
    CHECK(found);
}

namespace {
// Overwrite SystemModule's deviceName buffer through its bound control pointer —
// the same buffer the persistence overlay and an /api/control write target. Lets a
// test seed an invalid name and then drive the module's sanitisation.
void writeDeviceName(mm::SystemModule& sys, const char* value) {
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "deviceName") == 0) {
            char* buf = static_cast<char*>(sys.controls()[i].ptr);
            std::strncpy(buf, value, 23);
            buf[23] = 0;
            return;
        }
    }
    // No `deviceName` control found — a setup regression. Fail loudly rather than
    // silently no-op, which would let the calling test "pass" against a stale buffer.
    REQUIRE_MESSAGE(false, "writeDeviceName: no 'deviceName' control on SystemModule");
}
} // namespace

// deviceName is the single network identity, so SystemModule keeps it a valid hostname.
// A live edit to an invalid value ("My Room!") is coerced on the next loop1s tick
// (mm::sanitizeHostname), the same path mDNS/AP/DHCP read — so they never see spaces.
TEST_CASE("SystemModule sanitises a live deviceName edit") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();
    writeDeviceName(sys, "My Living Room!");
    sys.loop1s();                                   // the tick that coerces it
    CHECK(std::strcmp(sys.deviceName(), "My-Living-Room") == 0);
}

// An all-invalid name collapses to empty after sanitising; the MAC fallback then fills
// it, so deviceName is never empty (mDNS/AP/DHCP always have a name to register).
TEST_CASE("SystemModule falls back to the MAC name when deviceName is all-invalid") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();
    writeDeviceName(sys, "!@#$");
    sys.loop1s();
    CHECK(std::strcmp(sys.deviceName(), "MM-CAFE") == 0);   // desktop MAC fallback
}

// An already-valid name is left untouched (idempotent) — a normal user name survives.
TEST_CASE("SystemModule leaves a valid deviceName unchanged") {
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();
    writeDeviceName(sys, "Bench-S3");
    sys.loop1s();
    CHECK(std::strcmp(sys.deviceName(), "Bench-S3") == 0);
}

// (firmware identity controls — version / build / firmware — moved to FirmwareUpdateModule;
// see test/unit/core/unit_FirmwareUpdateModule.cpp.)

// The `bootReason` control is populated from platform::resetReason; on desktop it reports "OK".
TEST_CASE("SystemModule bootReason control populated") {
    // The bootReason control is wired in setup() (from platform::resetReason). On
    // desktop the platform stub always returns "OK". The UI uses this to set the
    // reboot button's crashed-state styling — see ui-spec.md.
    mm::SystemModule sys;
    sys.setup();
    sys.onBuildControls();

    bool found = false;
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "bootReason") == 0) {
            CHECK(sys.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(sys.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(val[0] != '\0');  // non-empty
            // Desktop stub always reports "OK"
            CHECK(std::strcmp(val, "OK") == 0);
            found = true;
        }
    }
    CHECK(found);
}

// System is fixed infrastructure — it accepts no user-added children (they live under
// the Services container). Its own children (Tasks, I2cScan) are wired by code.
TEST_CASE("SystemModule accepts no user-added children") {
    // System is fixed infrastructure: its children (Tasks, I2cScan) are wired by
    // code, so it accepts no user-added role. User-added capability modules live
    // under the Services container instead.
    mm::SystemModule sys;
    CHECK(std::strcmp(sys.acceptsChildRoles(), "") == 0);
}

// Regression: SystemModule overrides setup() and loop1s(); both must chain to
// MoonModule's base so a wired-by-code child's setup()/loop1s() actually fire.
// Without the chain a fixed child (Tasks/I2cScan) would never init or poll (the
// "children miss callbacks" trap from history/decisions.md). loop20ms() isn't
// overridden, so the base default already propagates it.
TEST_CASE("SystemModule propagates lifecycle to a wired-by-code child") {
    mm::SystemModule sys;
    CountingChild child;
    sys.addChild(&child);

    sys.setup();
    CHECK(child.setupCalls == 1);   // setup() chained to base

    sys.loop1s();
    CHECK(child.loop1sCalls == 1);  // loop1s() chained to base

    sys.loop20ms();
    CHECK(child.loop20msCalls == 1); // base default (not overridden) propagates
}

// roleName maps the Service enum to its lowercase API string.
TEST_CASE("Service role name") {
    CHECK(std::strcmp(mm::roleName(mm::ModuleRole::Service), "service") == 0);
}
