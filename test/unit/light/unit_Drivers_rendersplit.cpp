// @module Drivers
// @also platform

// Pins the multicore render↔encode split (Step 2a) on the host, where platform::spawnPinnedTask is a
// real std::thread — so the cross-core handoff invariants run on an actual second thread and TSan/ASan
// can catch a race or use-after-free on the shared outputBuffer_. The ESP32 core-1 task relies on
// exactly these invariants; here a MockDriver stands in for the real drivers (I80/Parlio are inert on
// the host — lanesAvailable()==0). One rule under test: while `multicore` is on, EVERY driver's tick()
// runs on core 1 against the finished frame; core 0 returns to rendering immediately.

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "light/layers/Layers.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"

#include <atomic>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

namespace {

// A driver stub that records what it reads from the source buffer each tick. `sawTear` latches if it
// ever reads the producer's mid-write sentinel (0xEE) — the proof the frame boundary held (core 0
// never overwrote outputBuffer_ while core 1 was reading it).
class MockDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer* b) override { src_ = b; }
    void tick() override {
        ticks.fetch_add(1);
        if (src_ && src_->data() && src_->count() > 0 && src_->data()[0] == 0xEE)
            sawTear.store(true);
    }
    mm::Buffer* src_ = nullptr;
    std::atomic<int> ticks{0};
    std::atomic<bool> sawTear{false};
};

// A driver that is SLOW inside tick() — it holds the worker inside its own tick() for ~40 ms, the way a
// real 16K-light encode holds core 1 for tens of ms. `inTick` is true exactly while the worker is
// executing this object's tick(); that is the window in which deleting it is a use-after-free.
class SlowDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() override {
        inTick.store(true);
        std::this_thread::sleep_for(40ms);   // stand-in for the encode: the worker is INSIDE this object
        touched = 0xABCD;                    // ASan traps here if core 0 freed us mid-tick
        inTick.store(false);
    }
    std::atomic<bool> inTick{false};
    uint16_t touched = 0;
};

// Two enabled Layers over a shared Layouts/Grid → needOutput is true (≥2 layers composite), so a real
// outputBuffer_ exists for the driver to read across the core boundary.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layers layers;
    mm::Layer layerA;
    mm::Layer layerB;
    mm::Drivers drivers;
    explicit Rig(uint16_t w) {
        grid.width = w; grid.height = 1; grid.depth = 1;
        layouts.addChild(&grid);
        layerA.setChannelsPerLight(3);
        layerB.setChannelsPerLight(3);
        layers.addChild(&layerA);
        layers.addChild(&layerB);
        layers.setLayouts(&layouts);   // propagates to the child layers
        drivers.setLayers(&layers);
        layers.applyState();           // sizes the layer buffers
    }
};

}  // namespace

TEST_CASE("render-split: multicore on → every driver ticks on the worker, never on a torn frame") {
    Rig r(64);
    MockDriver a, b;                          // two drivers: BOTH move to core 1, no per-driver opt-out
    r.drivers.addChild(&a);
    r.drivers.addChild(&b);
    r.drivers.setup();
    r.drivers.prepare();                      // engage predicate runs here

    REQUIRE(r.drivers.renderSplitActive());   // a driver exists + outputBuffer_ allocated → split ON

    for (int i = 0; i < 10; i++) {
        r.drivers.tick();                     // composites, notifies the worker
        std::this_thread::sleep_for(2ms);     // let the worker drain this frame
    }
    r.drivers.quiesce();               // wait the last tick out before asserting

    CHECK(a.ticks.load() >= 1);               // the worker ran BOTH drivers
    CHECK(b.ticks.load() >= 1);
    CHECK_FALSE(a.sawTear.load());            // never read a mid-write buffer — the boundary held
    CHECK_FALSE(b.sawTear.load());

    r.drivers.release();                      // stops + joins the worker (no hang, no leak)
    CHECK_FALSE(r.drivers.renderSplitActive());
}

TEST_CASE("render-split: multicore off → drivers tick inline on the render core (the proven path)") {
    Rig r(64);
    MockDriver d;
    r.drivers.addChild(&d);
    r.drivers.multicore = false;              // the user's switch
    r.drivers.setup();
    r.drivers.prepare();

    CHECK_FALSE(r.drivers.renderSplitActive());   // no split, no worker task
    for (int i = 0; i < 3; i++) r.drivers.tick();
    CHECK(d.ticks.load() == 3);               // ticked inline, once per frame, on this thread
    r.drivers.release();
}

TEST_CASE("render-split: no driver → the split does not engage (nothing to move)") {
    Rig r(64);
    r.drivers.setup();
    r.drivers.prepare();
    CHECK_FALSE(r.drivers.renderSplitActive());   // no output work → no task, no handoff buffer claim
    r.drivers.tick();                             // must not crash with no children
    r.drivers.release();
}

TEST_CASE("render-split: live disengage stops the worker when the last driver leaves") {
    Rig r(64);
    MockDriver d;
    r.drivers.addChild(&d);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());
    r.drivers.tick();
    std::this_thread::sleep_for(2ms);

    r.drivers.removeChild(&d);                // the last driver leaves
    r.drivers.prepare();                      // re-evaluate → disengage (worker stopped + joined)
    CHECK_FALSE(r.drivers.renderSplitActive());
    r.drivers.release();
}

// The delete-mid-encode race (Reviewer finding 1). The structural path — removeChild → release →
// deleteTree — used to run with NO quiesce: only prepare() waited for core 1, and it ran AFTER the
// child was already freed. So a UI delete landing while core 1 was inside that driver's tick() freed the
// object (and its DMA buffers) out from under the worker → LoadProhibited on ESP32.
//
// The fix is in core: MoonModule::removeChild() calls quiesce() (a no-op for a module with no worker,
// overridden by Drivers to wait the encode out), so the mutation cannot begin until core 1 is out.
// NOTE there is deliberately NO sleep before removeChild here — the whole point is to delete WHILE the
// worker is provably inside tick(). Under ASan a regression is a heap-use-after-free; without it, the
// assert below still catches the ordering.
TEST_CASE("render-split: deleting a driver WHILE core 1 is inside its tick() is safe (no use-after-free)") {
    Rig r(64);
    auto* slow = new SlowDriver();            // heap: so ASan can see a use-after-free if we regress
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    r.drivers.tick();                         // notifies core 1 → the worker enters slow->tick()

    // Wait only until the worker is provably INSIDE tick() — then mutate. This is the race window.
    while (!slow->inTick.load()) std::this_thread::yield();
    CHECK(slow->inTick.load());               // the worker is mid-encode, right now

    r.drivers.removeChild(slow);              // core's quiesce() must block here until the worker exits

    CHECK_FALSE(slow->inTick.load());         // removeChild returned only after the encode finished
    delete slow;                              // now safe — the worker is provably not inside it

    r.drivers.prepare();                      // last driver gone → disengage
    CHECK_FALSE(r.drivers.renderSplitActive());
    r.drivers.release();
}

TEST_CASE("render-split: toggling multicore live engages and disengages the worker") {
    Rig r(64);
    MockDriver d;
    r.drivers.addChild(&d);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());   // default ON

    r.drivers.multicore = false;              // the UI switch, applied live via the prepare sweep
    r.drivers.prepare();
    CHECK_FALSE(r.drivers.renderSplitActive());   // worker stopped + joined, back to inline
    const int inlineBefore = d.ticks.load();
    r.drivers.tick();
    CHECK(d.ticks.load() == inlineBefore + 1);    // ticked inline on this thread

    r.drivers.multicore = true;               // and back on again
    r.drivers.prepare();
    CHECK(r.drivers.renderSplitActive());     // worker respawned — no reboot needed
    r.drivers.release();
}
