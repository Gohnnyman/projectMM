// @module PreviewDriver

#include "doctest.h"
#include "core/Scheduler.h"
#include "light/drivers/PreviewDriver.h"
#include "light/drivers/Drivers.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/SphereLayout.h"

#include <vector>
#include <cstring>

// PreviewDriver streams a true-shape point list: a one-time 0x03 coordinate
// table (positions of the real lights) + per-frame 0x02 RGB indexed by light.
// These tests pin: the table carries exactly lightCount positions (sphere → its
// shell count, NOT the bounding box), the per-frame RGB count matches, and a
// large layout is index-downsampled (stride > 1) to fit the send-buffer cap.

namespace {

// Captures the two preview message types so tests can inspect them. Every message arrives through
// the ONE resumable send (sendBufferedFrame) as header ++ body, classified by the type byte:
// 0x03 tables (11-byte header, epoch at [10]) into lastCoord, 0x02 frames (9-byte header, epoch
// at [7], drops at [8]) into lastFrame. dropCoord/acceptNext make a send report "slot busy"
// (false) to drive the request-retry and drop-counting paths.
// -Wnon-virtual-dtor: BinaryBroadcaster's own destructor is protected and non-virtual on
// purpose ("not owned through this interface"), so no code can delete through a base pointer.
// This double is a stack local in every test, never owned polymorphically — and it cannot copy
// the base's protected-destructor trick, because that would forbid the stack construction the
// tests rely on. Scoped to this one type.
// #ifndef _MSC_VER: `#pragma GCC` is an unknown pragma to MSVC (C4068), and the Windows build
// runs /WX, so an unguarded one fails it. MSVC has no -Wnon-virtual-dtor equivalent to silence,
// so excluding it there is complete, not a workaround. Clang understands `#pragma GCC`.
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
struct CaptureBroadcaster : mm::BinaryBroadcaster {
    int coordMsgs = 0, frameMsgs = 0;
    std::vector<uint8_t> lastCoord, lastFrame;
    bool acceptNext = true;        // false → a color-frame send is refused (slot busy)
    bool dropCoord = false;        // true → a coord-table send is refused (slot busy)
    // The registered inbound-message sink (the driver): tests speak the pull protocol through it.
    ClientMessageSink* sink = nullptr;
    void setClientMessageSink(ClientMessageSink* s) override { sink = s; }
    // Convenience: a client in `slot` posts a standing [0x51][stride][fps] frame request.
    void ask(uint8_t stride, uint8_t fps = 0, int slot = 0) {
        const uint8_t m[3] = {0x51, stride, fps};
        if (sink) sink->onClientMessage(slot, m, 3);
    }
    // Convenience: a client asks for the coordinate table ([0x52][stride]).
    void askTable(uint8_t stride = 1, int slot = 0) {
        const uint8_t m[2] = {0x52, stride};
        if (sink) sink->onClientMessage(slot, m, 2);
    }
    // Single-threaded test transport: one producer thread, so there is no race to exclude — grant
    // unconditionally (the "may return true unconditionally" case in BinaryBroadcaster).
    bool tryAcquireSend() override { return true; }
    void releaseSend() override {}

    // The ONE resumable send: every /wsp message (0x03 tables and 0x02 frames alike) arrives
    // here, routed by its type byte. `bufferedDrains` models a slow link: the send stays "in
    // flight" for that many bufferedSendIdle() polls before going idle (0 = instant).
    // bufferedFrames counts accepted sends; bufferedDropped counts newest-wins backpressure drops.
    int bufferedFrames = 0, bufferedDropped = 0;
    int bufferedDrains = 0;            // ticks a send stays active (set >0 to model a slow link)
    int bufferedCanceled = 0;          // cancelBufferedSend() calls while a send was active
    const uint8_t* lastBody = nullptr; // body pointer of the in-flight send (for resize-safety test)
    bool sendBufferedFrame(const uint8_t* header, size_t headerLen,
                           const uint8_t* body, size_t bodyLen) override {
        if (active_) { bufferedDropped++; return false; }   // newest-wins backpressure
        const uint8_t type = headerLen ? header[0] : 0;
        if (type == 0x03) {
            if (dropCoord) return false;       // simulate the table send refused (slot busy)
            coordMsgs++;
            lastCoord.assign(header, header + headerLen);
            lastCoord.insert(lastCoord.end(), body, body + bodyLen);
        } else {
            if (!acceptNext) return false;     // simulate the color frame refused
            bufferedFrames++; frameMsgs++;
            lastFrame.assign(header, header + headerLen);
            lastFrame.insert(lastFrame.end(), body, body + bodyLen);
        }
        lastBody = body;
        remaining_ = bufferedDrains;       // >0 → stays "in flight" to model a slow link
        active_ = (remaining_ > 0);
        return true;
    }
    bool bufferedSendIdle() const override {
        if (active_ && remaining_ > 0) { --remaining_; if (remaining_ == 0) active_ = false; }
        return !active_;
    }
    void cancelBufferedSend() override { if (active_) bufferedCanceled++; active_ = false; }

    // 0x03 = [type][count:u32][bx][by][bz][stride:u16][epoch] (11-byte header)
    // 0x02 = [type][count:u32][stride:u16][epoch][drops] (9-byte header)
    static uint32_t u32le(const std::vector<uint8_t>& b, size_t o) {
        return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
    }
    int coordCount() const { return lastCoord.size() >= 5 ? static_cast<int>(u32le(lastCoord, 1)) : -1; }
    int frameCount() const { return lastFrame.size() >= 5 ? static_cast<int>(u32le(lastFrame, 1)) : -1; }
    int coordStride() const { return lastCoord.size() >= 11 ? lastCoord[8] | (lastCoord[9] << 8) : -1; }
    int coordEpoch() const { return lastCoord.size() >= 11 ? lastCoord[10] : -1; }
    int frameEpoch() const { return lastFrame.size() >= 9 ? lastFrame[7] : -1; }
    int frameDrops() const { return lastFrame.size() >= 9 ? lastFrame[8] : -1; }

private:
    mutable bool active_ = false;     // a buffered send is in flight
    mutable int remaining_ = 0;       // bufferedSendIdle polls left before it goes idle
};
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

// Wire PreviewDriver under Drivers, over a Layer + single layout, with a
// CaptureBroadcaster — the full real path (sparse driver buffer + layout coords).
struct PreviewRig {
    mm::Layouts group;
    mm::Layer layer;
    mm::Drivers drivers;
    mm::PreviewDriver* preview;   // owned by drivers' child array
    CaptureBroadcaster cap;

    PreviewRig(mm::LayoutBase* layout, uint8_t cpl = 3) {
        group.addChild(layout);
        layer.setLayouts(&group);
        layer.setChannelsPerLight(cpl);
        layer.defineControls();
        layer.applyState();

        preview = new mm::PreviewDriver();
        preview->setBroadcaster(&cap);
        drivers.addChild(preview);
        // Single-core: these cases pin PreviewDriver's own behavior (its downsample + the zero-copy
        // source), so keep the render↔encode split out of it — with multicore ON Drivers would own a
        // handoff buffer and preview would read THAT, not the layer's buffer, which is a different
        // (and separately tested) contract. See unit_Drivers_rendersplit.cpp.
        drivers.multicore = false;
        drivers.setLayer(&layer);          // passBufferToDrivers wires preview's source + layer
        drivers.defineControls();
        drivers.applyState();
    }

    void produce() {
        // Drives the build/send methods directly (bypassing tick), so no [0x52] is posted: a
        // standing tableRequested_ flag would leak into the tick-driven tests that follow.
        preview->buildCoordTable();
        preview->sendCoordTable();
        preview->sendFrame();
    }
};

} // namespace

// A sphere sends its SHELL lights (210), not the dense 9x9x9 box (729).
TEST_CASE("PreviewDriver coordinate table carries the real lights, not the box") {
    mm::SphereLayout s;
    s.radius = 4;                         // 210 shell lights, 9^3 box
    PreviewRig rig(&s);
    rig.produce();

    REQUIRE(rig.cap.coordMsgs > 0);
    CHECK(rig.cap.coordCount() == 210);   // the shell, not 729
    CHECK(rig.cap.coordStride() == 1);    // small → exact, no downsample
    // 0x03 = [0x03][count:u32][bx][by][bz][stride:u16][epoch] (11-byte hdr) + count*3 positions
    CHECK(rig.cap.lastCoord.size() == 11u + 210u * 3u);
}

// The device serves the resolution the CLIENT requests, it no longer measures the link itself
// (a device-side controller can only see its own socket, and cycled; the receiver measures the
// true end-to-end rate). The stride changes exactly when a request arrives, and never otherwise.
TEST_CASE("PreviewDriver adopts the client-requested stride, and only then") {
    mm::GridLayout g; g.width = 64; g.height = 64; g.depth = 1;
    PreviewRig rig(&g);

    uint32_t t = 1000;
    auto tickAt = [&](int n) { for (int i=0;i<n;i++){ t += 100; mm::platform::setTestNowMs(t); rig.preview->tick(); } };

    const int coordsBefore = rig.cap.coordMsgs;
    tickAt(50);                                       // 5 s with NO standing request: pure silence
    CHECK(rig.cap.frameMsgs == 0);
    CHECK(rig.cap.coordMsgs == coordsBefore);

    rig.cap.ask(4);                                   // a client asks for 1/4
    tickAt(1);
    CHECK(rig.preview->downscaleForTest() == 4);      // served exactly as requested

    tickAt(50);                                       // the request is STANDING, no drift back
    CHECK(rig.preview->downscaleForTest() == 4);
    CHECK(rig.cap.coordMsgs == coordsBefore);         // and no table was volunteered for it

    rig.cap.ask(1);                                   // the client asks for full detail again
    tickAt(1);
    CHECK(rig.preview->downscaleForTest() == 1);
    mm::platform::setTestNowMs(0);
}

// Garbage from the network must not steer the lattice: hints outside [1, 64] are ignored.
TEST_CASE("PreviewDriver ignores an out-of-range stride request") {
    mm::GridLayout g; g.width = 32; g.height = 32; g.depth = 1;
    PreviewRig rig(&g);
    rig.cap.ask(4);                                   // a real standing request first
    mm::platform::setTestNowMs(1000); rig.preview->tick();
    CHECK(rig.preview->downscaleForTest() == 4);
    rig.cap.ask(200);                                 // garbage: ignored at the store
    mm::platform::setTestNowMs(1100); rig.preview->tick();
    CHECK(rig.preview->downscaleForTest() == 4);      // the real request still stands
    mm::platform::setTestNowMs(0);
}

TEST_CASE("PreviewDriver per-frame RGB count matches the coordinate table") {
    mm::SphereLayout s;
    s.radius = 4;
    PreviewRig rig(&s);
    rig.produce();

    REQUIRE(rig.cap.frameMsgs > 0);
    CHECK(rig.cap.frameCount() == 210);
    // 0x02 = [0x02][count:u32][stride:u16][epoch][drops] (9-byte hdr) + count*3 RGB bytes
    CHECK(rig.cap.lastFrame.size() == 9u + 210u * 3u);
}

// A small grid sends every light at its grid position (stride 1, exact).
TEST_CASE("PreviewDriver small grid sends all lights exactly") {
    mm::GridLayout g;
    g.width = 8; g.height = 8; g.depth = 1;   // 64 lights
    PreviewRig rig(&g);
    rig.produce();

    CHECK(rig.cap.coordCount() == 64);
    CHECK(rig.cap.frameCount() == 64);
    CHECK(rig.cap.coordStride() == 1);
}

// A large layout is SPATIALLY downsampled (a regular per-axis lattice, not every-Nth-flat-
// index) so the payload fits the send-buffer cap without the diagonal moiré that linear
// stride produced on a grid whose width didn't divide the stride. The wire "stride" field
// carries the per-axis lattice/downscale factor (color k still maps 1:1 to coord k).
TEST_CASE("PreviewDriver downsamples on a regular spatial lattice when a client asks coarser") {
    // There is NO display cap: a host build (unlimited memory) serves any layout at full detail,
    // and coarseness exists only as a client REQUEST. Ask for 1/2 and pin the lattice geometry.
    // The extent (199) is ≤255/axis, so positions are sent at EXACT integer grid coordinates (no
    // byte-scaling rounding) — letting the regularity check below compare true lattice positions.
    mm::GridLayout g;
    g.width = 200; g.height = 200; g.depth = 1;
    PreviewRig rig(&g);
    rig.cap.ask(2);                               // the client requests 1/2
    mm::platform::setTestNowMs(2000); rig.preview->tick();   // adopt the request
    mm::platform::setTestNowMs(0);
    rig.produce();

    CHECK(rig.cap.coordStride() == 2);            // served exactly as asked
    CHECK(rig.cap.coordCount() == 100 * 100);     // ceil(200/2) per axis
    CHECK(rig.cap.coordCount() > 0);
    CHECK(rig.cap.coordCount() == rig.cap.frameCount());  // table + RGB agree (lockstep)

    // Regular lattice check: every sent X coordinate is a multiple of the same step, and so
    // is every Y — i.e. the kept points sit on a grid, with NO per-row column drift (the
    // diagonal-streak bug). Read the packed u8 positions back from the coord message.
    const auto& cd = rig.cap.lastCoord;
    const int hdr = 10;                           // [0x03][count:u32][bx][by][bz][stride:u16]
    REQUIRE(cd.size() >= static_cast<size_t>(hdr + 3));
    // Derive the X step from the first two distinct X values, then assert all X are multiples.
    int stepX = 0, x0 = cd[hdr];
    for (size_t p = hdr; p + 2 < cd.size(); p += 3) {
        int dx = cd[p] - x0;
        if (dx != 0) { stepX = dx > 0 ? dx : -dx; break; }
    }
    REQUIRE(stepX > 0);
    bool regular = true;
    for (size_t p = hdr; p + 2 < cd.size(); p += 3) {
        if (((cd[p] - x0) % stepX) != 0) { regular = false; break; }   // X off the lattice → drift
    }
    CHECK(regular);                               // no diagonal moiré
}

// A SPARSE layout under the cap must NOT be downsampled for its big BOUNDING BOX alone: the lattice
// bound is the layout's LIGHT count, not its box cell count, so a sphere whose shell fits the cap
// sends every light at stride 1 (a radius-8 sphere → ~812 shell lights, well under the 4096 display
// cap, in a 17³≈4913-cell box). (A genuinely huge sparse layout above the cap downsamples like any
// other — the cap is about points streamed, not box size.)
TEST_CASE("PreviewDriver keeps a sparse large-box layout at full resolution") {
    mm::SphereLayout s;
    s.radius = 8;                                 // big box (17³), shell light-count under the cap
    PreviewRig rig(&s);
    rig.produce();

    CHECK(rig.cap.coordCount() > 0);
    CHECK(rig.cap.coordCount() <= 4096);          // the shell fits the display cap...
    CHECK(rig.cap.coordStride() == 1);            // ...so it is sent whole, not downsampled
    CHECK(rig.cap.coordCount() == rig.cap.frameCount());
}

// Default fps is the rate-limited preview stream rate.
TEST_CASE("PreviewDriver targetFps default") {
    mm::PreviewDriver driver;
    CHECK(driver.targetFps == 24);
}

// Regression: a coordinate table dropped under backpressure must be RETRIED, and color
// frames withheld until it lands — otherwise the device sends 0x02 frames the browser skips
// (count mismatch) and the preview freezes for the whole session. Drives tick() (where the
// coord-pending logic lives) with a broadcaster that drops every 0x03, then lets it through.
TEST_CASE("a table request outranks frames, is retried while refused, and frames then resume") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;   // 256 lights, full res
    PreviewRig rig(&g);
    rig.cap.frameMsgs = 0;                     // ignore any frame from rig construction
    rig.cap.coordMsgs = 0;
    rig.cap.ask(1);                            // a viewer wants frames...
    rig.cap.askTable();                        // ...and asked for the positions first
    rig.cap.dropCoord = true;                  // but every table send is refused (slot busy)

    uint32_t t = 1000;
    auto tick = [&] { t += 100; mm::platform::setTestNowMs(t); rig.preview->tick(); };

    // Pump tick(). The owed 0x03 outranks frames, so while it cannot go out, NOTHING does: a
    // 0x02 now would carry a count the asker cannot map.
    for (int i = 0; i < 5; i++) tick();
    CHECK(rig.cap.frameMsgs == 0);
    CHECK(rig.cap.coordMsgs == 0);

    rig.cap.dropCoord = false;                 // the slot frees
    tick();                                    // the owed table lands
    tick();                                    // and frames resume
    CHECK(rig.cap.coordMsgs == 1);             // sent exactly once, not spammed
    CHECK(rig.cap.frameMsgs > 0);
    CHECK(rig.cap.coordCount() == rig.cap.frameCount());

    mm::platform::setTestNowMs(0);
}

// Regression: deleting the active Layer must not leave a driver holding a
// dangling layer_ pointer. Previously Drivers::passBufferToDrivers early-returned
// when the active Layer was null, leaving PreviewDriver's layer_ pointing at the
// freed Layer; the next prepare read layer_->layouts() on freed memory and
// crashed the device (LoadProhibited → boot loop, since the broken tree persists).
// Now passBufferToDrivers clears the drivers' layer_/sourceBuffer_ to null, a safe
// idle state. This drives the real path: Drivers bound to a Effects CONTAINER
// (self-healing), the Layer removed, then prepareTree re-resolves activeLayer()=null.
TEST_CASE("PreviewDriver tolerates the active Layer being deleted") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;
    mm::Layouts group; group.addChild(&g);
    mm::Effects layers;
    auto* layer = new mm::Layer();
    layer->setChannelsPerLight(3);
    layers.addChild(layer);
    layers.setLayouts(&group);
    layers.defineControls();

    mm::Drivers drivers;
    auto* preview = new mm::PreviewDriver();
    CaptureBroadcaster cap;
    preview->setBroadcaster(&cap);
    drivers.addChild(preview);
    drivers.setEffects(&layers);          // container-bound: layer_ re-resolved at prepareTree
    drivers.defineControls();

    layers.applyState();
    drivers.applyState();
    REQUIRE(preview->layer() == layer);  // wired to the active Layer

    // Remove the only Layer, then rebuild — activeLayer() now returns null.
    layers.removeChild(layer);
    layer->release();
    mm::Scheduler::deleteTree(layer);    // free it — a stale pointer would now dangle

    layers.applyState();
    drivers.applyState();              // must NOT deref the freed Layer
    CHECK(preview->layer() == nullptr);  // cleared, not dangling

    // And producing a frame on the empty pipeline is a safe no-op (no crash).
    preview->buildCoordTable();
    preview->sendFrame();
    CHECK(cap.frameMsgs == 0);           // nothing to send with no layer
}

// The pull model: a coordinate table is sent ONLY when a client asks ([0x52]), never on a
// timer, never per-frame, never volunteered on a connect or a geometry change (the device is a
// dumb producer; a client whose cache misses asks). Driven through tick() with a frozen clock.
TEST_CASE("PreviewDriver sends the coordinate table only when a client asks, never on its own") {
    mm::platform::setTestNowMs(100000);
    PreviewRig rig(new mm::GridLayout(), 3);
    rig.cap.ask(1);                      // a standing frame request, but NO table request
    rig.cap.coordMsgs = 0;

    // 3 seconds of frames: the table is never volunteered.
    for (int t = 1; t <= 30; t++) {
        mm::platform::setTestNowMs(100000 + t * 100);
        rig.preview->tick();
    }
    CHECK(rig.cap.frameMsgs > 0);
    CHECK(rig.cap.coordMsgs == 0);

    // The client asks: the next tick answers, exactly once.
    rig.cap.askTable();
    mm::platform::setTestNowMs(104100); rig.preview->tick();
    mm::platform::setTestNowMs(104200); rig.preview->tick();
    CHECK(rig.cap.coordMsgs == 1);

    mm::platform::setTestNowMs(0);       // restore the real clock for other tests
}

// A full-res RGB frame is sent through the RESUMABLE buffered path (sendBufferedFrame), whose body
// is the DRIVER (consumer) buffer itself — no copy. For a dense identity grid that's the Layer's
// dense box buffer; for a sparse/mapped layout it's the LUT-mapped output buffer (the real lights),
// the same buffer the LED drivers consume — NOT the dense box.
TEST_CASE("PreviewDriver routes a dense full-res frame through the resumable buffered send") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;   // dense, no LUT (identity)
    PreviewRig rig(&g);
    rig.cap.bufferedFrames = 0;
    rig.produce();
    CHECK(rig.cap.bufferedFrames == 1);                      // went through sendBufferedFrame
    CHECK(rig.cap.frameCount() == 256);                      // the full grid, full res
    CHECK(rig.cap.frameCount() == rig.cap.coordCount());     // table + frame agree
    CHECK(rig.cap.lastBody == rig.layer.buffer().data());    // body IS the dense box buffer (no copy)
}

// Sparse layout: the buffered send streams the LUT-mapped DRIVER buffer (only the real lights, in
// driver order), exactly like the LED drivers — NOT the dense bounding box. So coordCount == the
// shell count and the frame is sent whole at full res through the resumable path.
TEST_CASE("PreviewDriver buffered send uses the sparse driver buffer, not the dense box") {
    mm::SphereLayout s; s.radius = 4;            // 210 shell lights in a 9^3 = 729 box
    PreviewRig rig(&s);
    rig.cap.bufferedFrames = 0;
    rig.produce();
    CHECK(rig.cap.bufferedFrames == 1);                      // full res → resumable buffered send
    CHECK(rig.cap.frameCount() == 210);                      // the SHELL, not 729 — the driver buffer
    CHECK(rig.cap.frameCount() == rig.cap.coordCount());     // table + frame agree (lockstep)
    CHECK(rig.cap.lastBody != rig.layer.buffer().data());    // NOT the dense box — the mapped output
}

// Dense-grid CLOSED-FORM downsample, exact color placement: a wide strip pinned over the cap
// strides in x only, so the kept lights are columns 0,s,2s,… The color pass must read each from its
// dense buffer index (closed-form x for a 1-row grid) and pack them in the SAME order as the coord
// table: no placeLights. Painting a known color at a kept column and finding it at the matching
// frame position pins the index math + the lattice order.
TEST_CASE("PreviewDriver dense downsample packs colors by closed-form index, in lattice order") {
    const int width = 20000;
    mm::GridLayout g; g.width = width; g.height = 1; g.depth = 1;
    PreviewRig rig(&g);
    rig.cap.ask(3);                                    // the client requests 1/3 (no cap forces one)
    mm::platform::setTestNowMs(2000); rig.preview->tick();
    mm::platform::setTestNowMs(0);
    rig.produce();                                     // ask for + receive the table (pull model)
    const int s = rig.cap.coordStride();
    REQUIRE(s == 3);                                   // served exactly as asked
    const int kept = rig.cap.coordCount();
    REQUIRE(kept == (width + s - 1) / s);              // ceil(width/s) — closed-form count

    // Paint the 2nd kept column (x = s) bright green; the rest black.
    uint8_t* buf = rig.layer.buffer().data();
    std::memset(buf, 0, static_cast<size_t>(width) * 3);
    buf[s * 3 + 1] = 150;                              // G at column x=s (the 2nd kept light)

    rig.preview->sendFrame();
    // 0x02 = 7-byte hdr + (r,g,b)×kept. The 2nd kept light is the 2nd triple → its G byte at 7+3+1.
    REQUIRE(rig.cap.lastFrame.size() == 9u + static_cast<size_t>(kept) * 3u);
    CHECK(rig.cap.lastFrame[9 + 3 + 1] == 150);        // painted column landed at the 2nd position
    CHECK(rig.cap.lastFrame[9 + 1] == 0);              // column 0 is black (1st position)

    mm::platform::setTestMaxAllocBlock(0);
}

// ADAPTIVE FRAME RATE: while a buffered send is still draining (a slow link), tick() must NOT start
// a new frame — it waits for bufferedSendIdle(). So the effective rate self-limits to the link.
TEST_CASE("PreviewDriver gates the next frame on the buffered send draining (adaptive fps)") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;
    PreviewRig rig(&g);
    rig.cap.ask(1);                    // a standing request: the pull model serves only the asked
    rig.cap.bufferedDrains = 3;        // each send stays "in flight" for 3 idle-polls (slow link)

    uint32_t t = 1000;
    auto tick = [&] { t += 100; mm::platform::setTestNowMs(t); rig.preview->tick(); };

    tick();                            // first loop: coord table + first buffered frame starts
    const int after1 = rig.cap.bufferedFrames;
    CHECK(after1 == 1);
    tick();                            // send still draining → must NOT start a second frame
    tick();
    CHECK(rig.cap.bufferedFrames == after1);   // gated: no new frame while busy
    CHECK(rig.cap.bufferedDropped == 0);       // it WAITED, didn't spam-and-drop

    mm::platform::setTestNowMs(0);
}

TEST_CASE("PreviewDriver cancels an in-flight buffered send on rebuild (resize safety)") {
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;
    PreviewRig rig(&g);
    rig.cap.bufferedDrains = 10;       // keep a send "in flight" so a rebuild interrupts it

    rig.preview->sendFrame();          // start a buffered send (stays active for 10 polls)
    CHECK(rig.cap.bufferedFrames >= 1);
    const int cancelsBefore = rig.cap.bufferedCanceled;

    // A resize: rebuild the pipeline. PreviewDriver::prepare must cancel the active send
    // BEFORE the buffer is reallocated.
    g.width = 32; g.height = 32;
    rig.layer.applyState();          // reallocs the producer buffer (the body the send pointed at)
    rig.preview->applyState();       // must cancel the in-flight send

    CHECK(rig.cap.bufferedCanceled == cancelsBefore + 1);   // the stale send was cancelled
}

TEST_CASE("a wedged link never blocks a tick, never closes a client, and resumes when it drains") {
    // The step-1 contract of the lean transport: the producer can only ARM messages; every socket
    // byte moves on the transport tick at TCP's pace. A link that stops draining therefore holds
    // the preview (one frame parked in the slot), costs the render loop nothing, and, since the
    // broadcaster interface has no per-client close at all, the producer CANNOT disconnect anyone.
    mm::GridLayout g; g.width = 16; g.height = 16; g.depth = 1;
    PreviewRig rig(&g);
    rig.cap.ask(1);                    // a standing request: the pull model serves only the asked
    rig.cap.bufferedDrains = 1000;     // effectively wedged: stays in flight for 1000 idle-polls

    const int tableAtSetup = rig.cap.coordMsgs;   // the rig's prepare already delivered the table
    uint32_t t = 1000;
    for (int i = 0; i < 100; i++) { t += 100; mm::platform::setTestNowMs(t); rig.preview->tick(); }
    CHECK(rig.cap.bufferedFrames == 1);            // ONE frame parked in the slot, nothing spammed
    CHECK(rig.cap.coordMsgs == tableAtSetup);      // and no table churn either
    CHECK(rig.cap.bufferedDropped == 0);           // held, not spam-and-dropped

    rig.cap.bufferedDrains = 0;        // the link recovers
    while (!rig.cap.bufferedSendIdle()) {}
    const int before = rig.cap.bufferedFrames;
    for (int i = 0; i < 3; i++) { t += 100; mm::platform::setTestNowMs(t); rig.preview->tick(); }
    CHECK(rig.cap.bufferedFrames > before);        // and the stream simply resumes

    mm::platform::setTestNowMs(0);
}
