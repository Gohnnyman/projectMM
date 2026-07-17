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
#include <mutex>
#include <condition_variable>
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

// A driver the test can PARK inside its own tick(), standing in for a real 16K-light encode that holds
// core 1 for tens of ms. The handshake is a condition variable, not a sleep: `entered` fires the moment
// the worker is inside tick(), and the worker then blocks until the test releases it. So the test drives
// the race window deterministically instead of hoping a 40 ms sleep is long enough — no timing luck, and
// no unbounded spin that could hang the suite forever.
class SlowDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() override {
        {
            std::unique_lock<std::mutex> lk(m);
            inTick = true;
            entered.notify_all();            // "core 1 is now INSIDE this object"
            // Hold here until the test says go. This is the use-after-free window, held open on demand.
            release.wait_for(lk, 5s, [this] { return released; });
        }
        touched = 0xABCD;                    // ASan traps here if core 0 freed us mid-tick
        std::lock_guard<std::mutex> lk(m);
        inTick = false;
        exited.notify_all();
    }
    // Block until the worker is provably inside tick(). Bounded: a false return means it never got
    // there, which the caller asserts on rather than spinning forever.
    bool waitEntered() {
        std::unique_lock<std::mutex> lk(m);
        return entered.wait_for(lk, 5s, [this] { return inTick; });
    }
    void letGo() {
        { std::lock_guard<std::mutex> lk(m); released = true; }
        release.notify_all();
    }
    bool isInTick() { std::lock_guard<std::mutex> lk(m); return inTick; }

    std::mutex m;
    std::condition_variable entered, release, exited;
    bool inTick = false, released = false;
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

// THE INVARIANT: core quiesces the worker before any structural mutation of a container's children.
// MoonModule::removeChild() calls quiesce() — a no-op for a module with no worker, overridden by Drivers
// to wait out the in-flight encode — so a mutation cannot begin while core 1 is inside a child's tick().
// Violate it and the sequence removeChild → release → deleteTree frees the driver (and its DMA buffers)
// out from under the worker mid-encode: a use-after-free, LoadProhibited on ESP32.
//
// The test deletes the driver at the one instant that is unsafe: while the worker is provably inside its
// tick(). Under ASan a regression is a heap-use-after-free; without ASan, the ordering assert still
// catches it (removeChild must not return until the worker is out).
TEST_CASE("render-split: deleting a driver WHILE core 1 is inside its tick() is safe (no use-after-free)") {
    Rig r(64);
    auto* slow = new SlowDriver();            // heap: so ASan can see a use-after-free if we regress
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    r.drivers.tick();                         // notifies core 1 → the worker enters slow->tick()

    // Park the worker INSIDE tick() — a latch, not a sleep, so the race window is opened on demand
    // rather than by timing luck, and a worker that never starts fails here instead of spinning forever.
    REQUIRE(slow->waitEntered());
    CHECK(slow->isInTick());                  // the worker is mid-encode, right now

    // A releaser lets the parked encode finish shortly AFTER the delete is under way — the real
    // sequence, where the encode completes on its own while core 0 is already mutating the tree.
    // `releasedAt` records WHEN, so the assertion below can prove removeChild() actually waited for it.
    std::atomic<bool> releaseFired{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(30ms);    // long enough that a non-waiting removeChild returns first
        releaseFired.store(true);
        slow->letGo();
    });

    r.drivers.removeChild(slow);              // core's quiesce() blocks here until the worker is out

    // THE ASSERTION, checked the instant removeChild returns — not after the join, or a non-waiting
    // removeChild would look correct once the worker later finished on its own. With quiesce(),
    // removeChild cannot return until the worker exited, which cannot happen before letGo() fired.
    CHECK(releaseFired.load());               // it waited for the encode (fails without quiesce)
    CHECK_FALSE(slow->isInTick());            // and the worker is provably out of tick()

    releaser.join();
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

// ROBUSTNESS FLOOR: a wedged core-1 worker must not hang the RENDER loop. The frame boundary waits for
// the encode, normally bounded by one encode (the `renderWait` KPI measures it). But a worker that never
// signals done — starved, wedged, a lost notify — would otherwise spin core 0 forever, and a permanent
// wedge ranks BELOW "degraded": the device must keep running, even poorly. So the boundary times out,
// DISENGAGES the split, and every driver falls back to ticking inline on core 0 — the same single-core
// path a memory-tight board already takes. Slower, still lit.
//
// This times tick()'s boundary specifically. It does NOT go through quiesce() (the structural-mutation
// hook), which deliberately JOINS the worker on timeout — a blocking join is right there (the caller is
// about to free the driver) but would be wrong here, on the render path.
TEST_CASE("render-split: a wedged worker degrades to single-core instead of hanging the render loop") {
    Rig r(64);
    auto* slow = new SlowDriver();            // parks inside tick() until we release it
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    r.drivers.tick();                         // core 1 enters slow->tick() and parks there
    REQUIRE(slow->waitEntered());             // it is provably stuck

    // Release the worker FIRST so the disengage path can't block on it — what is under test is the
    // BOUNDARY giving up, not the teardown. The worker still hasn't signalled done for the frame the
    // boundary is waiting on, so the wait below is a genuine wedge from the boundary's point of view.
    const auto t0 = std::chrono::steady_clock::now();
    const bool completed = r.drivers.quiesceEncodeForTest();   // the boundary wait, in isolation
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK_FALSE(completed);                           // it TIMED OUT rather than spinning forever
    CHECK(elapsed < 2s);                              // and gave up promptly (timeout is 500 ms)
    CHECK_FALSE(r.drivers.renderSplitActive());       // disengaged: drivers now tick inline on core 0
    CHECK(r.drivers.status() != nullptr);             // and the user is told why

    slow->letGo();                            // let the parked worker unwind
    r.drivers.release();                      // joins it
    delete slow;
}
