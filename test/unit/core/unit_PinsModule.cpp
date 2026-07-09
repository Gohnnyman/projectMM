// @module PinsModule

// Pins PinsModule's one job: walk the live module tree, collect every claimed GPIO (a
// ControlType::Pin control set >= 0, plus the LED-driver "pins" CSV), and expose them as a
// GPIO-keyed read-only list — owner + name-derived role. A `-1` pin is unused and skipped;
// a GPIO claimed twice stays visible (both owners in the row detail), which is the read-only
// surfacing phase-1 does instead of enforcing. No platform reach — it reads the tree, so the
// host test covers the whole module, not just an empty path.

#include "doctest.h"
#include "core/PinsModule.h"
#include "core/Scheduler.h"
#include "core/JsonSink.h"

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
