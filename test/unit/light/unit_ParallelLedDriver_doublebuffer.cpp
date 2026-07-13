// @module ParallelLedDriver
// @also LcdLedDriver, ParlioLedDriver

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"

#include <cstring>
#include <vector>

// Host test of the deferred-wait DOUBLE-BUFFER logic in ParallelLedDriver::tick()
// (Step 1.5). The real LCD/Parlio peripherals are inert on the host (desktop stubs
// return null), so the alternation/wait/drain invariants can't be exercised through
// them. Instead a MockDriver supplies the CRTP bus* hooks against two in-memory
// buffers and RECORDS the call sequence, so the base loop's behaviour is pinned on
// the host exactly where the hardware would run it:
//   - double-buffer mode alternates encode target 0,1,0,1,… and waits on a buffer
//     only right before it's REUSED (never after every transmit);
//   - single-buffer mode (mock offers no buffer 1) stays on buffer 0 and waits every
//     frame — the old synchronous behaviour, unchanged fps;
//   - a reinit/release drains BOTH buffers' in-flight transfers before freeing.
// The fps win itself is a hardware KPI (proven on the P4); this pins the mechanism.

namespace {

using mm::nrOfLightsType;

// A record of one bus call, so a test can assert the exact ordering the base emits.
struct Call {
    enum Kind { Transmit, Wait } kind;
    uint8_t buffer;
};

// CRTP peripheral mock: two heap buffers, a settable "double-buffer available" flag,
// and a call log. Everything the base's tick()/reinit()/deinit() reach is here.
class MockParallelDriver : public mm::ParallelLedDriver<MockParallelDriver> {
public:
    // --- test knobs / observation ---
    bool twoBuffers = true;               // false → single-buffer mode (busBuffer(1) == null)
    std::vector<Call> calls;              // transmit/wait order, in call sequence
    uint8_t activeForTest() const { return active_; }
    bool inFlightForTest(uint8_t i) const { return inFlight_[i]; }

    // --- CRTP hooks (mock, host-only) ---
    static constexpr uint8_t lanesAvailable() { return 8; }   // pretend this chip has lanes
    static constexpr bool kExactLaneCount = false;
    static constexpr bool kLoopbackFullWidth = false;
    static constexpr const char* kInitFailMsg = "mock init failed";

    void addBusControls() {}
    bool busControlTriggersBuild(const char*) const { return false; }
    void recordBusPins() {}
    bool extraBusPinsCurrent() const { return true; }
    const char* validateBusPins(const uint16_t*, uint8_t) const { return nullptr; }
    const char* validateBusFatal() const { return nullptr; }

    // busInit gets `wantSecond` from the base (= asyncTransmit). The mock allocates the second
    // buffer only when BOTH the flag wants it AND the test's twoBuffers knob allows it (so a test
    // can simulate a memory-tight board that refuses the second buffer even with async on).
    bool busInit(size_t frameBytes, bool wantSecond) {
        cap_ = frameBytes;
        buf_[0].assign(frameBytes, 0);
        if (wantSecond && twoBuffers) buf_[1].assign(frameBytes, 0);
        else                          buf_[1].clear();
        inited_ = true;
        return true;
    }
    uint8_t* busBuffer(uint8_t i) {
        if (i >= 2 || buf_[i].empty()) return nullptr;
        return buf_[i].data();
    }
    size_t busCapacity() const { return cap_; }
    bool busTransmit(uint8_t i, size_t /*bytes*/) {
        calls.push_back({Call::Transmit, i});
        return true;   // the mock transfer always "starts"
    }
    void busWait(uint8_t i, uint32_t) { calls.push_back({Call::Wait, i}); }
    uint32_t busLastTransmitUs() const { return lastTransmitUs; }   // mock wire-time KPI
    uint32_t lastTransmitUs = 0;   // a test can set this to check the wireUs string formatting
    void busDeinit() { cap_ = 0; buf_[0].clear(); buf_[1].clear(); inited_ = false; }
    mm::platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) {
        return {};
    }

private:
    std::vector<uint8_t> buf_[2];
    size_t cap_ = 0;
    bool inited_ = false;
};

// Wire a mock driver onto a `lights`-light source buffer + a GRB correction, and drive it ready.
// `async` sets asyncTransmit (whether the base requests a second buffer); `canSecond` is the mock's
// board-fits-a-second-buffer knob (lets a test simulate a memory-tight board that refuses it even
// with async on). Mirrors the other parallel-driver test helpers.
void wire(MockParallelDriver& d, mm::Buffer& src, mm::Correction& corr,
          nrOfLightsType lights, bool async, bool canSecond = true) {
    d.twoBuffers = canSecond;
    d.asyncTransmit = async;
    std::strcpy(d.pins, "1,2,3,4");
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

// Double-buffer mode: the encode target alternates 0,1,0,1,… and a buffer's wait
// fires only right BEFORE that buffer is reused — never after every transmit. So
// the first two ticks transmit without a preceding wait (both buffers start idle),
// and from tick 3 on each tick waits on the buffer it's about to reuse.
TEST_CASE("ParallelLedDriver double-buffer alternates and defers the wait") {
    MockParallelDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64, /*async=*/true);

    // Tick 1: buffer 0 idle → no wait, transmit 0, flip to 1.
    d.tick();
    CHECK(d.activeForTest() == 1);
    CHECK(d.inFlightForTest(0) == true);
    // Tick 2: buffer 1 idle → no wait, transmit 1, flip to 0.
    d.tick();
    CHECK(d.activeForTest() == 0);
    CHECK(d.inFlightForTest(1) == true);
    // Tick 3: about to reuse buffer 0 (in flight from tick 1) → wait 0, transmit 0.
    d.tick();
    CHECK(d.activeForTest() == 1);
    // Tick 4: reuse buffer 1 → wait 1, transmit 1.
    d.tick();
    CHECK(d.activeForTest() == 0);

    // The exact call order the base emitted.
    REQUIRE(d.calls.size() == 6);
    CHECK(d.calls[0].kind == Call::Transmit); CHECK(d.calls[0].buffer == 0);
    CHECK(d.calls[1].kind == Call::Transmit); CHECK(d.calls[1].buffer == 1);
    CHECK(d.calls[2].kind == Call::Wait);     CHECK(d.calls[2].buffer == 0);
    CHECK(d.calls[3].kind == Call::Transmit); CHECK(d.calls[3].buffer == 0);
    CHECK(d.calls[4].kind == Call::Wait);     CHECK(d.calls[4].buffer == 1);
    CHECK(d.calls[5].kind == Call::Transmit); CHECK(d.calls[5].buffer == 1);
}

// Single-buffer mode (no second buffer): the driver stays on buffer 0 and waits on
// it EVERY frame before re-encoding — the old synchronous wait-after-transmit path,
// so a memory-tight board keeps its old fps rather than failing to init.
TEST_CASE("ParallelLedDriver single-buffer mode waits every frame on buffer 0") {
    MockParallelDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64, /*async=*/false);   // default: no second buffer, synchronous path

    d.tick();   // tickSync: transmit 0, wait 0
    CHECK(d.activeForTest() == 0);
    d.tick();   // transmit 0, wait 0
    CHECK(d.activeForTest() == 0);
    d.tick();   // transmit 0, wait 0

    // tickSync waits RIGHT AFTER each transmit (the original synchronous order): T0,W0 ×3.
    REQUIRE(d.calls.size() == 6);
    CHECK(d.calls[0].kind == Call::Transmit); CHECK(d.calls[0].buffer == 0);
    CHECK(d.calls[1].kind == Call::Wait);     CHECK(d.calls[1].buffer == 0);
    CHECK(d.calls[2].kind == Call::Transmit); CHECK(d.calls[2].buffer == 0);
    CHECK(d.calls[3].kind == Call::Wait);     CHECK(d.calls[3].buffer == 0);
    CHECK(d.calls[4].kind == Call::Transmit); CHECK(d.calls[4].buffer == 0);
    CHECK(d.calls[5].kind == Call::Wait);     CHECK(d.calls[5].buffer == 0);
    // Buffer 1 is never allocated or touched.
    for (const auto& c : d.calls) CHECK(c.buffer == 0);
}

// asyncTransmit is the on/off knob AND drives allocation: OFF (default) allocates ONE buffer and
// runs the synchronous path; ON requests a second buffer and alternates. Flipping it rebuilds the
// bus (affectsPrepare) so the second buffer is freed (→off) or allocated (→on) — a board that leaves
// it off never holds the second buffer. This mirrors the live toggle (the A/B knob), which routes
// through applyState()/prepare() the same way.
TEST_CASE("ParallelLedDriver asyncTransmit toggles allocation and path") {
    MockParallelDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64, /*async=*/false);

    // OFF: single buffer only — buffer 1 was never allocated, and tick runs synchronous.
    CHECK(d.busBuffer(1) == nullptr);
    d.tick();
    REQUIRE(d.calls.size() == 2);   // T0, W0 — synchronous
    CHECK(d.calls[0].kind == Call::Transmit);
    CHECK(d.calls[1].kind == Call::Wait);

    // Flip ON and re-prepare (what a live control change does): the second buffer is now allocated.
    d.asyncTransmit = true;
    d.applyState();
    CHECK(d.busBuffer(1) != nullptr);
    d.calls.clear();
    d.tick();   // async: transmit 0, no wait, flip to 1
    d.tick();   // async: transmit 1, no wait, flip to 0
    CHECK(d.activeForTest() == 0);
    // Two transmits, no interleaved wait (both buffers idle at start) — the deferred-wait pattern.
    REQUIRE(d.calls.size() == 2);
    CHECK(d.calls[0].kind == Call::Transmit); CHECK(d.calls[0].buffer == 0);
    CHECK(d.calls[1].kind == Call::Transmit); CHECK(d.calls[1].buffer == 1);

    // Flip back OFF and re-prepare: the second buffer is freed, back to synchronous.
    d.asyncTransmit = false;
    d.applyState();
    CHECK(d.busBuffer(1) == nullptr);
}

// A board that WANTS async but can't fit the second buffer (memory-tight) degrades to single-buffer
// synchronous — never fails to init. asyncTransmit is on, but the mock refuses the second buffer.
TEST_CASE("ParallelLedDriver async degrades to synchronous when second buffer won't fit") {
    MockParallelDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64, /*async=*/true, /*canSecond=*/false);
    CHECK(d.busBuffer(1) == nullptr);   // requested but didn't fit
    d.tick();
    REQUIRE(d.calls.size() == 2);       // synchronous path (T0, W0)
    CHECK(d.calls[0].kind == Call::Transmit);
    CHECK(d.calls[1].kind == Call::Wait);
}

// The wireUs KPI: tick1s() pulls the platform's measured wire time via busLastTransmitUs(). The
// string formatting + the actual DMA timing are verified on hardware (the metric's whole point is a
// real wire measurement); here we just pin that tick1s reads the seam without crashing pre-first-frame.
TEST_CASE("ParallelLedDriver wireUs tick1s is safe before the first transfer") {
    MockParallelDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64, /*async=*/true);
    d.tick1s();                 // lastTransmitUs == 0 → placeholder path, must not divide by zero
    d.lastTransmitUs = 7680;    // the 256-light WS2812 floor
    d.tick1s();                 // real path (1e6/7680 = 130 fps) — must not crash
    CHECK(d.busLastTransmitUs() == 7680);
}

// Robustness + no-caps (regression for a live bootloop): a correction can carry ANY channel count
// (RGB=3, RGBW=4, RGBCCT=5, an N-channel fixture). The per-row encode scratch (wire_) is heap-sized to
// kMaxLanes × outChannels off the hot path, so a >4-channel correction lays out without overrun — the
// old fixed-4-byte-stride array overflowed and corrupted memory (the SE16 bootloop, 2026-07-13). The
// driver must DRIVE it (size the frame + encode), not idle and not crash.
TEST_CASE("ParallelLedDriver drives an N-channel (>4) correction without overflow") {
    using R = mm::ChannelRole;
    MockParallelDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.asyncTransmit = true;
    std::strcpy(d.pins, "1,2,3,4");
    REQUIRE(src.allocate(64, 3) == true);
    // An 8-channel fixture-style correction (RGBW + 4 fixture roles) — well over the old 4-byte slot.
    R roles[] = {R::Red, R::Green, R::Blue, R::White, R::Pan, R::Tilt, R::Dimmer, R::Zoom};
    corr.rebuild(255, roles, 8);
    CHECK(corr.outChannels == 8);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();

    // No error — the driver accepts the multi-channel correction and sizes a real frame (outCh=8 →
    // frameBytes scales with 8). The encode runs and transmits; the ASan/valgrind-clean run (and the
    // hardware regression on the SE16) is the overflow proof — a 4-byte stride would have corrupted
    // memory here.
    CHECK(d.severity() != MockParallelDriver::Severity::Error);
    CHECK(d.frameBytes() > 0);
    // The status reports the total channel count for a multi-channel fixture (lights × channels) —
    // the DMX-universe footprint the user sizes against, not just the light count.
    CHECK(std::string(d.status()).find("(") != std::string::npos);   // "... (N channels)"
    CHECK(std::string(d.status()).find("channels") != std::string::npos);
    d.calls.clear();
    d.tick();
    REQUIRE(d.calls.size() >= 1);
    CHECK(d.calls[0].kind == Call::Transmit);   // it actually drove the fixture
}

// A reinit (grid resize / pin edit) must drain BOTH buffers' in-flight transfers
// before freeing them — a live DMA reading a buffer about to be freed is a
// use-after-free. After two ticks both buffers are in flight; the resize's reinit
// waits on both before rebuilding. (async on → two buffers.)
TEST_CASE("ParallelLedDriver reinit drains both in-flight buffers") {
    MockParallelDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64, /*async=*/true);

    d.tick();   // transmit 0 (in flight)
    d.tick();   // transmit 1 (in flight)
    CHECK(d.inFlightForTest(0) == true);
    CHECK(d.inFlightForTest(1) == true);
    const size_t before = d.calls.size();

    // Force a rebuild by growing the grid, which changes frameBytes → reinit().
    REQUIRE(src.allocate(256, 3) == true);
    d.setSourceBuffer(&src);
    d.applyState();   // re-parses + reinits; must drain both first

    // Both buffers were waited on during the drain (order-independent — assert both present).
    bool waited0 = false, waited1 = false;
    for (size_t i = before; i < d.calls.size(); i++) {
        if (d.calls[i].kind == Call::Wait && d.calls[i].buffer == 0) waited0 = true;
        if (d.calls[i].kind == Call::Wait && d.calls[i].buffer == 1) waited1 = true;
    }
    CHECK(waited0);
    CHECK(waited1);
    // After the drain both flags are clear (the rebuild starts fresh on buffer 0).
    CHECK(d.inFlightForTest(0) == false);
    CHECK(d.inFlightForTest(1) == false);
    CHECK(d.activeForTest() == 0);
}
