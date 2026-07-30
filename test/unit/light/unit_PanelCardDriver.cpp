// @module PanelCardDriver
// @also Drivers, Correction

// End-to-end tests for the raw-Ethernet panel driver. The desktop platform records every frame
// ethSendRaw() emits instead of putting it on a wire, so these pin what the driver would actually
// send — the geometry, the chunking, and the sync-last ordering — with no hardware and no
// privileges. The bench then only has to confirm the wire itself.

#include "doctest.h"
#include "light/drivers/PanelCardDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/PanelsLayout.h"

namespace {

// Build a driver over a `count`-light source with a plain RGB correction, ready to tick.
// Build a driver over a `count`-light source. With no Layer wired the driver falls back to the
// panel arrangement for the wall size, which is exactly what a bare unit test wants: geometry it
// states rather than geometry it has to construct a Layout for.
// A wall of `width` x `height`, wired the way production wires it: a Layout gives the Layer its
// physical size, and the driver reads that. The driver has no geometry of its own to set.
struct Wall {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    Wall(uint16_t width, uint16_t height) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
    }
};

void setUp(mm::PanelCardDriver& driver, mm::Buffer& source, Wall& wall,
           mm::nrOfLightsType count) {
    REQUIRE(source.allocate(count, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    driver.setLayer(&wall.layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::ethTestClearFrames();
}

// Drop any raw-L2 claim left by an earlier test's driver. Test drivers are stack objects that are
// destroyed without release() being called, so the claim count would otherwise carry across cases.
void clearClaims() {
    while (mm::platform::ethRawL2Claimed()) mm::platform::ethClaimRawL2(false);
}

// The packet-type byte of a captured frame (offset 12), which says row / sync / brightness.
uint8_t frameType(size_t i) {
    return mm::platform::ethTestFrameData(i)[12];
}

}  // namespace

// A panel is sent as one frame per row followed by a single sync — the sync is what latches the
// image, so it must arrive after every row it applies to.
TEST_CASE("PanelCardDriver sends one frame per row then one sync") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 4);
    setUp(driver, source, wall, 256);

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 8);   // 2 brightness + 4 rows + 2 sync
    CHECK(frameType(0) == mm::COLORLIGHT_TYPE_BRIGHTNESS);
    CHECK(frameType(1) == mm::COLORLIGHT_TYPE_BRIGHTNESS);
    for (size_t i = 2; i <= 5; i++) CHECK(frameType(i) == mm::COLORLIGHT_TYPE_ROW);
    CHECK(frameType(6) == mm::COLORLIGHT_TYPE_SYNC);
    CHECK(frameType(7) == mm::COLORLIGHT_TYPE_SYNC);
}

// A buffer smaller than the wall sends only the rows it covers, then latches — rather than reading
// past the buffer for the rows it does not have.
TEST_CASE("PanelCardDriver stops at the last row its buffer covers") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 8);            // wall is 8 rows high
    setUp(driver, source, wall, 128);   // buffer holds only 2 rows of it

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 6);   // 2 brightness + 2 rows + 2 sync, not 8 rows
    CHECK(frameType(5) == mm::COLORLIGHT_TYPE_SYNC);
}

// A row wider than one packet splits into several, each carrying its own pixel offset — the wide-
// panel case, where 497 pixels is the per-packet ceiling.
TEST_CASE("PanelCardDriver splits a row wider than one packet") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(600, 1);           // one row of 600 > the 497-pixel packet ceiling
    setUp(driver, source, wall, 600);

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 6);   // 2 brightness + 2 row packets + 2 sync
    // First chunk: offset 0, a full 497 pixels.
    const uint8_t* a = mm::platform::ethTestFrameData(2);
    CHECK(((a[15] << 8) | a[16]) == 0);
    CHECK(((a[17] << 8) | a[18]) == mm::COLORLIGHT_MAX_PIXELS_PER_PACKET);
    // Second chunk: continues at 497, carrying the remaining 103.
    const uint8_t* b = mm::platform::ethTestFrameData(3);
    CHECK(((b[15] << 8) | b[16]) == mm::COLORLIGHT_MAX_PIXELS_PER_PACKET);
    CHECK(((b[17] << 8) | b[18]) == 600 - mm::COLORLIGHT_MAX_PIXELS_PER_PACKET);
}

// Pixel bytes reach the wire unchanged, so what an effect rendered is what the panel receives.
TEST_CASE("PanelCardDriver puts rendered pixels on the wire") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(2, 1);
    setUp(driver, source, wall, 2);
    uint8_t* px = source.data();
    px[0] = 10; px[1] = 20; px[2] = 30;
    px[3] = 40; px[4] = 50; px[5] = 60;

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 5);   // 2 brightness + row + 2 sync
    const uint8_t* row = mm::platform::ethTestFrameData(2);
    CHECK(row[mm::COLORLIGHT_ROW_PREFIX + 0] == 10);
    CHECK(row[mm::COLORLIGHT_ROW_PREFIX + 1] == 20);
    CHECK(row[mm::COLORLIGHT_ROW_PREFIX + 2] == 30);
    CHECK(row[mm::COLORLIGHT_ROW_PREFIX + 3] == 40);
}

// The fps control caps the send rate, so a render loop faster than the panel needs doesn't
// saturate the link.
TEST_CASE("PanelCardDriver rate-limits to its fps setting") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);
    driver.fps = 40;   // one frame every 25 ms

    mm::platform::setTestNowMs(1000);
    driver.tick();
    const size_t after1 = mm::platform::ethTestFrameCount();
    CHECK(after1 == 5);   // 2 brightness + row + 2 sync

    mm::platform::setTestNowMs(1010);   // too soon
    driver.tick();
    CHECK(mm::platform::ethTestFrameCount() == after1);

    mm::platform::setTestNowMs(1030);   // past the interval
    driver.tick();
    CHECK(mm::platform::ethTestFrameCount() > after1);
}

// Nothing is latched when the window is empty: an all-zero geometry must not emit a bare sync,
// which would blank a panel that another driver is feeding.
TEST_CASE("PanelCardDriver sends nothing when the window is empty") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);
    driver.fps = 0;   // nothing to send

    mm::platform::setTestNowMs(1000);
    driver.tick();

    CHECK(mm::platform::ethTestFrameCount() == 0);
}

// A dropped frame doesn't stall the driver: the cards give no acknowledgement, so a failed send is
// tolerated exactly as a dropped UDP packet is, and the next tick proceeds.
TEST_CASE("PanelCardDriver survives a failing link") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    mm::platform::setTestEthSendFails(true);
    mm::platform::setTestNowMs(1000);
    driver.tick();                                   // must not hang or crash
    CHECK(mm::platform::ethTestFrameCount() == 0);   // nothing reached the wire

    mm::platform::setTestEthSendFails(false);
    mm::platform::setTestNowMs(1100);
    driver.tick();
    CHECK(mm::platform::ethTestFrameCount() == 5);   // recovers on the next tick
}

// The correction-applied buffer is sized off the hot path, so tick() never allocates — the same
// contract the other sinks hold.
TEST_CASE("PanelCardDriver sizes its buffer in prepare, not in tick") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    REQUIRE(driver.correctedBuffer().data() != nullptr);
    CHECK(driver.correctedBuffer().count() == 64);

    const uint8_t* before = driver.correctedBuffer().data();
    mm::platform::setTestNowMs(1000);
    driver.tick();
    CHECK(driver.correctedBuffer().data() == before);
    CHECK(driver.correctedBuffer().count() == 64);
}

// The driver CLAIMS the ethernet interface while it is enabled, which is how NetworkModule tells
// "the cable is broken" apart from "a driver is using the wire below IP". Stated at prepare rather
// than inferred from traffic, so the claim is in place before the first frame and cannot race the
// network cascade's DHCP timeout.
TEST_CASE("PanelCardDriver claims the ethernet link while enabled") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    CHECK_FALSE(mm::platform::ethRawL2Claimed());     // nothing claimed before setup

    Wall wall(64, 1);
    setUp(driver, source, wall, 64);                  // applyState() runs prepare()
    CHECK(mm::platform::ethRawL2Claimed());           // claimed without a frame being sent

    driver.release();
    CHECK_FALSE(mm::platform::ethRawL2Claimed());     // released with the driver
}

// Re-preparing (a geometry or window edit) must not stack claims, or the link would stay claimed
// after the driver is gone.
TEST_CASE("PanelCardDriver claim survives a re-prepare without stacking") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    driver.prepare();
    driver.prepare();
    CHECK(mm::platform::ethRawL2Claimed());

    driver.release();
    CHECK_FALSE(mm::platform::ethRawL2Claimed());   // one release is enough
}

// The wall a PanelsLayout describes is what reaches the card: two 128x64 panels stacked give 128
// rows of 128 pixels, and the driver reads that from the Layout rather than from controls of its
// own. This is the setup a ColorLight card is normally configured for.
TEST_CASE("PanelCardDriver sends the wall a PanelsLayout describes") {
    clearClaims();
    mm::Layouts layouts;
    mm::PanelsLayout panels;
    panels.horizontalPanels = 1;
    panels.verticalPanels = 2;        // stacked
    panels.panelWidth = 128;
    panels.panelHeight = 64;
    layouts.addChild(&panels);
    layouts.applyState();

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.applyState();
    REQUIRE(layer.physicalWidth() == 128);
    REQUIRE(layer.physicalHeight() == 128);

    mm::Buffer source;
    REQUIRE(source.allocate(128 * 128, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    mm::PanelCardDriver driver;
    driver.setLayer(&layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::ethTestClearFrames();

    mm::platform::setTestNowMs(1000);
    driver.tick();

    // 2 brightness + 128 rows (one packet each, 128 <= 497) + 2 sync
    REQUIRE(mm::platform::ethTestFrameCount() == 132);
    // Rows are numbered across the whole card, not restarted per panel.
    const uint8_t* firstRow = mm::platform::ethTestFrameData(2);
    REQUIRE(firstRow != nullptr);
    CHECK(((firstRow[13] << 8) | firstRow[14]) == 0);
    const uint8_t* lastRow = mm::platform::ethTestFrameData(129);
    REQUIRE(lastRow != nullptr);
    CHECK(((lastRow[13] << 8) | lastRow[14]) == 127);
}

// Panels chained side by side widen each card row instead of adding rows — the same Layout control
// the user already sets, with no second place to state it.
TEST_CASE("PanelCardDriver widens the row when a PanelsLayout chains panels across") {
    clearClaims();
    mm::Layouts layouts;
    mm::PanelsLayout panels;
    panels.horizontalPanels = 2;      // chained: 256 px per row
    panels.verticalPanels = 1;
    panels.panelWidth = 128;
    panels.panelHeight = 64;
    layouts.addChild(&panels);
    layouts.applyState();

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.applyState();
    REQUIRE(layer.physicalWidth() == 256);

    mm::Buffer source;
    REQUIRE(source.allocate(256 * 64, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    mm::PanelCardDriver driver;
    driver.setLayer(&layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::ethTestClearFrames();

    mm::platform::setTestNowMs(1000);
    driver.tick();

    // 2 brightness + 64 rows + 2 sync — still one packet per row, since 256 <= 497.
    REQUIRE(mm::platform::ethTestFrameCount() == 68);
    const uint8_t* row = mm::platform::ethTestFrameData(2);
    REQUIRE(row != nullptr);
    CHECK(((row[17] << 8) | row[18]) == 256);   // pixels in this packet
}

// A transmit path that refuses every frame must SAY so. It reads as a healthy link otherwise — the
// IDF driver keeps reporting the negotiated speed while rejecting every frame — so "0 packets/s at
// 1 Gbit" would look identical to an idle effect.
TEST_CASE("PanelCardDriver reports a failing transmit path instead of a healthy link") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    mm::platform::setTestEthSendFails(true);
    mm::platform::setTestNowMs(1000);
    driver.tick();
    driver.tick1s();
    CHECK(mm::platform::ethSendFailStreak() > 0);

    mm::platform::setTestEthSendFails(false);
    mm::platform::setTestNowMs(1100);
    driver.tick();
    CHECK(mm::platform::ethSendFailStreak() == 0);   // a success clears it: back-pressure is not a fault
}
