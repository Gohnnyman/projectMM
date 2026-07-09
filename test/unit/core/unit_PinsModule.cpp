// @module PinsModule

// Pins PinsModule's one job: walk the live module tree, collect every claimed GPIO (a
// ControlType::Pin control set >= 0, plus the LED-driver "pins" CSV), and expose them as a
// GPIO-keyed read-only list — owner + name-derived role + a severity flag when the claim lands
// on an unsafe pin. A `-1` pin is unused and skipped; a GPIO claimed twice stays visible (both
// owners in the row detail), which is the read-only surfacing this phase does instead of enforcing.
// Severity reads platform::gpioCapability, which desktop stubs to "all safe" — so the severity
// cases inject a fake capability via setTestGpioCapability (as unit_TasksModule injects a snapshot).

#include "doctest.h"
#include "core/PinsModule.h"
#include "core/Scheduler.h"
#include "core/JsonSink.h"
#include "platform/platform.h"   // setTestGpioCapability — inject an unsafe pin for the severity cases

#include <cstring>
#include <string>

using namespace mm;

namespace {

// A module that stakes GPIO claims: some Pin controls and (optionally) a "pins" CSV, exactly the
// two claim shapes a real driver/mic/PHY exposes. Controls bind to members, so the Pin values are
// read back live off ptr the way PinsModule reads them.
struct FakePinModule : MoonModule {
    int8_t sck = -1, ws = -1, sd = -1, tx = -1;
    char pins[32] = {};
    bool withPinsCsv = false;

    FakePinModule(const char* n) { setName(n); }

    void onBuildControls() override {
        MoonModule::onBuildControls();
        controls_.addPin("sckPin", sck);
        controls_.addPin("wsPin", ws);
        controls_.addPin("sdPin", sd);
        controls_.addPin("loopbackTxPin", tx);
        if (withPinsCsv) controls_.addText("pins", pins, sizeof(pins));
    }
};

// Reach the `pins` control's ListSource so the row/detail JSON is exercised directly (same access
// unit_TasksModule uses: descriptor.ptr → ListSource*).
const ListSource* pinsSource(const MoonModule& m) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, "pins") == 0)
            return static_cast<const ListSource*>(m.controls()[i].ptr);
    return nullptr;
}

// The whole list serialized, row by row — a cheap way to assert presence/absence of a GPIO.
std::string allRows(const ListSource& src) {
    std::string out;
    for (uint8_t i = 0; i < src.listRowCount(); i++) {
        JsonSink r; src.writeListRow(r, i);
        out += r.data();
    }
    return out;
}

} // namespace

TEST_CASE("PinsModule: exposes a single read-only pins list, fixed System module (Generic role)") {
    PinsModule pins;
    pins.onBuildControls();
    bool hasList = false;
    for (uint8_t i = 0; i < pins.controls().count(); i++)
        if (std::strcmp(pins.controls()[i].name, "pins") == 0) hasList = true;
    CHECK(hasList);
    // Wired-by-code System child (main.cpp), not user-added → base Generic role, so no container
    // accepts it as a user-editable child and the UI shows no delete.
    CHECK(pins.role() == ModuleRole::Generic);
}

TEST_CASE("PinsModule: collects set Pin controls with name-derived roles, skips -1") {
    Scheduler scheduler;
    FakePinModule mic("Audio");
    mic.sck = 13; mic.ws = 14; mic.sd = 15;   // tx left at -1 → unused, must be skipped
    PinsModule pins;
    scheduler.addModule(&mic);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();   // walk the tree, build the map

    const ListSource* src = pinsSource(pins);
    REQUIRE(src != nullptr);
    REQUIRE(src->listRowCount() == 3);   // 13/14/15, NOT the -1 loopbackTxPin

    const std::string rows = allRows(*src);
    CHECK(rows.find("\"gpio\":13") != std::string::npos);
    CHECK(rows.find("\"gpio\":14") != std::string::npos);
    CHECK(rows.find("\"gpio\":15") != std::string::npos);
    CHECK(rows.find("loopback") == std::string::npos);   // the -1 pin never appears

    // Roles are name-derived: sckPin→BCLK, wsPin→WS, sdPin→data; owner is the module name.
    CHECK(rows.find("\"role\":\"BCLK\"") != std::string::npos);
    CHECK(rows.find("\"role\":\"WS\"") != std::string::npos);
    CHECK(rows.find("\"role\":\"data\"") != std::string::npos);
    CHECK(rows.find("\"owner\":\"Audio\"") != std::string::npos);
}

TEST_CASE("PinsModule: parses the LED-driver pins CSV into per-lane claims") {
    Scheduler scheduler;
    FakePinModule driver("RmtLed");
    driver.withPinsCsv = true;
    std::strcpy(driver.pins, "18,19,20");
    PinsModule pins;
    scheduler.addModule(&driver);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const ListSource* src = pinsSource(pins);
    REQUIRE(src != nullptr);
    REQUIRE(src->listRowCount() == 3);

    const std::string rows = allRows(*src);
    CHECK(rows.find("\"gpio\":18") != std::string::npos);
    CHECK(rows.find("\"gpio\":19") != std::string::npos);
    CHECK(rows.find("\"gpio\":20") != std::string::npos);
    // The lanes are numbered from the CSV order.
    CHECK(rows.find("\"role\":\"LED lane 0\"") != std::string::npos);
    CHECK(rows.find("\"role\":\"LED lane 1\"") != std::string::npos);
    CHECK(rows.find("\"role\":\"LED lane 2\"") != std::string::npos);
}

TEST_CASE("PinsModule: rows are GPIO-ordered and a double-claim stays visible in the detail") {
    // Two modules both claim GPIO 21 — the conflict phase-1 surfaces (not enforces). Give them
    // out-of-order pins so the sort is exercised too.
    Scheduler scheduler;
    FakePinModule a("Alpha");
    a.sck = 21; a.ws = 9;         // 21 (BCLK) + 9 (WS)
    FakePinModule b("Beta");
    b.sd = 21;                    // also claims 21 (data) — the double-claim
    PinsModule pins;
    scheduler.addModule(&a);
    scheduler.addModule(&b);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const ListSource* src = pinsSource(pins);
    REQUIRE(src != nullptr);
    REQUIRE(src->listRowCount() == 3);   // 9, 21, 21

    // GPIO-ordered: row 0 is the lowest pin (9).
    JsonSink r0; src->writeListRow(r0, 0);
    CHECK(std::string(r0.data()).find("\"gpio\":9") != std::string::npos);

    // The detail for a GPIO-21 row lists BOTH claimants — the visible conflict.
    JsonSink detail; src->writeListRowDetail(detail, 1);
    const std::string det(detail.data());
    CHECK(det.find("Alpha \xC2\xB7 BCLK") != std::string::npos);
    CHECK(det.find("Beta \xC2\xB7 data") != std::string::npos);

    // #3 conflict soft-flag: BOTH GPIO-21 rows are flagged severity error (the summary goes red, not
    // just the detail); the lone GPIO-9 row is not.
    JsonSink r1; src->writeListRow(r1, 1);
    JsonSink r2; src->writeListRow(r2, 2);
    CHECK(std::string(r1.data()).find("\"severity\":\"error\"") != std::string::npos);
    CHECK(std::string(r2.data()).find("\"severity\":\"error\"") != std::string::npos);
    CHECK(std::string(r0.data()).find("\"severity\"") == std::string::npos);   // GPIO 9, single owner
}

TEST_CASE("PinsModule: a conflict promotes a strap warn to error (severity is the max)") {
    platform::clearTestGpioCapability();
    platform::GpioCapability strap;
    strap.strap = true;
    platform::setTestGpioCapability(45, strap);   // GPIO 45 is a strap → an LED lane there would be warn

    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.withPinsCsv = true;
    std::strcpy(drv.pins, "45");
    FakePinModule other("Other");
    other.sck = 45;                                // a SECOND claim on the strap pin → conflict
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&other);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    // Both GPIO-45 rows: the conflict (error) wins over the strap (warn) — severity is the max.
    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"severity\":\"error\"") != std::string::npos);
    CHECK(rows.find("\"severity\":\"warn\"") == std::string::npos);   // promoted, not left at warn
    platform::clearTestGpioCapability();
}

TEST_CASE("PinsModule: a disabled module's pins are released from the map, re-claimed on enable") {
    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.sck = 17;
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&pins);
    scheduler.setup();

    // Enabled: the claim shows.
    pins.loop1s();
    CHECK(allRows(*pinsSource(pins)).find("\"gpio\":17") != std::string::npos);

    // Disabled: the pin is freed from the map (switching a module off releases its GPIOs, no reboot).
    drv.setEnabled(false);
    pins.loop1s();
    CHECK(allRows(*pinsSource(pins)).find("\"gpio\":17") == std::string::npos);
    CHECK(pinsSource(pins)->listRowCount() == 0);

    // Re-enabled: the claim comes back.
    drv.setEnabled(true);
    pins.loop1s();
    CHECK(allRows(*pinsSource(pins)).find("\"gpio\":17") != std::string::npos);
}

TEST_CASE("PinsModule: a child module's pins are walked (depth-first), not just the roots") {
    // A driver nested under a container still contributes its claims — the walk recurses children.
    Scheduler scheduler;
    FakePinModule container("Layout");
    FakePinModule child("NestedDriver");
    child.sck = 42;
    container.addChild(&child);
    PinsModule pins;
    scheduler.addModule(&container);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const ListSource* src = pinsSource(pins);
    REQUIRE(src != nullptr);
    const std::string rows = allRows(*src);
    CHECK(rows.find("\"gpio\":42") != std::string::npos);
    CHECK(rows.find("\"owner\":\"NestedDriver\"") != std::string::npos);
}

TEST_CASE("PinsModule: a claim survives its owning module being destroyed (no use-after-free)") {
    // The map refreshes on loop1s but the UI serializes state right after a delete op — so a claim
    // must NOT borrow a pointer into the (now-freed) module's name storage. Refresh with the module
    // alive, destroy it, then serialize: the snapshot must still render its owner from its own copy.
    Scheduler scheduler;
    PinsModule pins;
    {
        FakePinModule doomed("Doomed");
        doomed.sck = 7;
        scheduler.addModule(&doomed);
        scheduler.addModule(&pins);
        scheduler.setup();
        pins.loop1s();   // claim {gpio 7, owner "Doomed"} captured here
    }   // `doomed` destroyed — its name_[16] storage is gone; a borrowed owner pointer would dangle

    const ListSource* src = pinsSource(pins);
    REQUIRE(src != nullptr);
    REQUIRE(src->listRowCount() == 1);
    // Serialize AFTER the owner is gone — reads the copied owner, not freed memory.
    JsonSink r0; src->writeListRow(r0, 0);
    const std::string row0(r0.data());
    CHECK(row0.find("\"gpio\":7") != std::string::npos);
    CHECK(row0.find("\"owner\":\"Doomed\"") != std::string::npos);
    // The detail path reads owner too — exercise it as well.
    JsonSink d0; src->writeListRowDetail(d0, 0);
    CHECK(std::string(d0.data()).find("Doomed \xC2\xB7 BCLK") != std::string::npos);
}

TEST_CASE("PinsModule: an out-of-range CSV pin is skipped, not wrapped to a false GPIO") {
    // parsePinList accepts up to 65535; a typo like "300" must not truncate to GPIO 44 in the map.
    Scheduler scheduler;
    FakePinModule driver("RmtLed");
    driver.withPinsCsv = true;
    std::strcpy(driver.pins, "18,300,19");   // 300 is past MM_MAX_GPIO — dropped
    PinsModule pins;
    scheduler.addModule(&driver);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const ListSource* src = pinsSource(pins);
    REQUIRE(src != nullptr);
    const std::string rows = allRows(*src);
    CHECK(rows.find("\"gpio\":18") != std::string::npos);
    CHECK(rows.find("\"gpio\":19") != std::string::npos);
    CHECK(rows.find("\"gpio\":44") == std::string::npos);   // 300 & 0xFF = 44 — must NOT appear
    CHECK(src->listRowCount() == 2);                        // only the two valid pins
}

// --- severity flagging (increment #2) ---------------------------------------------------------
// gpioCapability is stubbed "all safe" on desktop, so inject an unsafe capability for one gpio and
// assert PinsModule grades the claim: reserved→error, driven-role-on-strap/input-only→warn, else none.

TEST_CASE("PinsModule: a claim on a reserved pin is flagged severity error") {
    platform::clearTestGpioCapability();
    platform::GpioCapability reserved;   // reserved flash/PSRAM pin
    reserved.reserved = true;
    platform::setTestGpioCapability(30, reserved);

    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.sck = 30;                         // any role on a reserved pin is an error
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"gpio\":30") != std::string::npos);
    CHECK(rows.find("\"severity\":\"error\"") != std::string::npos);
    platform::clearTestGpioCapability();
}

TEST_CASE("PinsModule: a driven role on a strap pin is flagged severity warn") {
    platform::clearTestGpioCapability();
    platform::GpioCapability strap;       // a valid, output-capable, but strapping pin
    strap.strap = true;
    platform::setTestGpioCapability(45, strap);

    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.withPinsCsv = true;
    std::strcpy(drv.pins, "45");           // an LED lane drives the pin → warn on a strap
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"gpio\":45") != std::string::npos);
    CHECK(rows.find("\"severity\":\"warn\"") != std::string::npos);
    platform::clearTestGpioCapability();
}

TEST_CASE("PinsModule: an input role on an input-only pin is NOT flagged") {
    platform::clearTestGpioCapability();
    platform::GpioCapability inputOnly;   // valid but no output driver (classic ESP32 34-39)
    inputOnly.outputCapable = false;
    platform::setTestGpioCapability(34, inputOnly);

    Scheduler scheduler;
    FakePinModule mic("Audio");
    mic.sd = 34;                           // a mic data line READS the pin — input-only is fine
    PinsModule pins;
    scheduler.addModule(&mic);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"gpio\":34") != std::string::npos);
    CHECK(rows.find("\"severity\"") == std::string::npos);   // input role on input-only = safe
    platform::clearTestGpioCapability();
}

TEST_CASE("PinsModule: a driven role on an input-only pin IS flagged warn") {
    platform::clearTestGpioCapability();
    platform::GpioCapability inputOnly;
    inputOnly.outputCapable = false;
    platform::setTestGpioCapability(34, inputOnly);

    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.withPinsCsv = true;
    std::strcpy(drv.pins, "34");           // an LED lane can't drive an input-only pin → warn
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"severity\":\"warn\"") != std::string::npos);
    platform::clearTestGpioCapability();
}

TEST_CASE("PinsModule: a safe pin carries no severity field") {
    platform::clearTestGpioCapability();   // no override → desktop reports all safe
    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.sck = 18;
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"gpio\":18") != std::string::npos);
    CHECK(rows.find("\"severity\"") == std::string::npos);   // safe → no field, no colour
}

// --- live state (increment #4) ----------------------------------------------------------------
// gpioLiveState is stubbed valid=false on desktop (no real pins), so inject a live state and assert
// PinsModule emits the level/drive columns; a pin with no live state omits them.

TEST_CASE("PinsModule: a claimed pin with live state emits level + drive columns") {
    platform::clearTestGpioLiveState();
    platform::GpioLiveState live;
    live.valid = true; live.level = true; live.driveCap = 2;   // HIGH, STRONG
    platform::setTestGpioLiveState(18, live);

    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.sck = 18;
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"level\":\"HIGH\"") != std::string::npos);
    CHECK(rows.find("\"drive\":\"STRONG\"") != std::string::npos);
    platform::clearTestGpioLiveState();
}

TEST_CASE("PinsModule: a pin with no live state (valid=false) omits the live columns") {
    platform::clearTestGpioLiveState();   // no override → desktop stub returns valid=false
    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.sck = 18;
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"gpio\":18") != std::string::npos);
    CHECK(rows.find("\"level\"") == std::string::npos);   // no live state → no columns
    CHECK(rows.find("\"drive\"") == std::string::npos);
}

// --- live direction (dir column) --------------------------------------------------------------
// gpioLiveState carries the pad's live output/input enable. The map shows a `dir` column
// (out/in/both/off) as INFORMATION — it does not auto-warn (see the "dir is shown as info" case).

TEST_CASE("PinsModule: dir column reflects the live pad direction (out/in/both/off)") {
    platform::clearTestGpioLiveState();
    auto live = [](bool o, bool i) {
        platform::GpioLiveState s; s.valid = true; s.output = o; s.input = i; return s;
    };
    platform::setTestGpioLiveState(10, live(true, false));    // out
    platform::setTestGpioLiveState(11, live(false, true));    // in
    platform::setTestGpioLiveState(12, live(true, true));     // both (e.g. open-drain I²C)
    platform::setTestGpioLiveState(13, live(false, false));   // off

    Scheduler scheduler;
    FakePinModule m("Drv");
    m.sck = 10; m.ws = 11; m.sd = 12; m.tx = 13;   // sck/ws/sd/tx map to controls; values are the gpios
    PinsModule pins;
    scheduler.addModule(&m);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"gpio\":10") != std::string::npos);
    CHECK(rows.find("\"dir\":\"out\"") != std::string::npos);
    CHECK(rows.find("\"dir\":\"in\"") != std::string::npos);
    CHECK(rows.find("\"dir\":\"both\"") != std::string::npos);
    CHECK(rows.find("\"dir\":\"off\"") != std::string::npos);
    platform::clearTestGpioLiveState();
}

TEST_CASE("PinsModule: dir is shown as info, NOT a warning — a driven role with output off is unflagged") {
    // The live direction is informational only: too many pins are legitimately not-driving-when-idle
    // (I²C is bidirectional, a loopback Tx is off until the self-test runs, an RMII clock can be an
    // input), so `dir` reading in/off must NOT auto-warn — that would be noise. Only the static
    // capability flags (reserved / strap / input-only) drive severity.
    platform::clearTestGpioLiveState();
    platform::GpioLiveState notDriving;   // a driven role's pin reads output-off — but this is NOT flagged
    notDriving.valid = true; notDriving.output = false; notDriving.input = true;
    platform::setTestGpioLiveState(14, notDriving);

    Scheduler scheduler;
    FakePinModule drv("RmtLed");
    drv.withPinsCsv = true;
    std::strcpy(drv.pins, "14");
    PinsModule pins;
    scheduler.addModule(&drv);
    scheduler.addModule(&pins);
    scheduler.setup();
    pins.loop1s();

    const std::string rows = allRows(*pinsSource(pins));
    CHECK(rows.find("\"gpio\":14") != std::string::npos);
    CHECK(rows.find("\"dir\":\"in\"") != std::string::npos);   // direction is shown...
    CHECK(rows.find("\"severity\"") == std::string::npos);     // ...but NOT flagged (no false positive)
    platform::clearTestGpioLiveState();
}
