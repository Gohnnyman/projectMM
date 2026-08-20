#pragma once

#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveScript.h"
#include "light/layouts/LayoutBase.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstdio>
#include <cstring>

// MoonLiveLayout — a scripted LAYOUT: where the lights physically are, written as text on a running
// device instead of compiled in as a C++ class.
//
// A layout is the one part of the pipeline that differs for every physical build — a ring, a spiral
// staircase, a car grille, a costume. Each one has meant a new C++ class, a rebuild and a reflash.
// A script means the person who hung the lights can describe where they went, on the device, and
// see it immediately.
//
// This is the binding that needed the language to grow. A modifier's script transforms ONE
// coordinate because the Layer calls it once per light; a layout has no such per-light call to ride
// on — it has to place N lights itself, which takes a loop. That is why `for` landed first.
//
// **It allocates nothing, like every other layout.** `GridLayout` computes its count arithmetically
// and emits straight into the sink; `SphereLayout` walks the same loop twice, counting then
// emitting, "so they never disagree". A scripted layout does exactly the same: the script calls
// `addLight(x, y, z)` per light, and the binding points that call at a counter on the sizing pass
// and at the consumer's sink on the walk. Staging the coordinates in an array instead would cost
// 48 KB on a 16k-light fixture — memory a classic ESP32 does not have, and a mechanism no other
// layout uses, which matters because scripted and compiled layouts compose in one `Layouts`
// container and have to behave identically through this interface.
//
// **The count and the coordinates come from the same code.** `lightCount()` runs the script with a
// counting sink; `placeLights` runs it again into the caller's. Same script, same arithmetic, so
// the two answers cannot drift apart — the property SphereLayout's comment names.

namespace mm {

/// Layout whose physical light positions are a live-authored MoonLive script.
class MoonLiveLayout : public LayoutBase {
public:
    const char* tags() const override { return "📝"; }   // scripted

    void defineControls() override {
        // The script NAME, not the script — the text lives in a file the UI loads and saves
        // through /api/file. A module costs ~32 bytes here instead of a resident kilobyte.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kLayoutPick);
        // Every control the SCRIPT declared — including any extents it loops over. A layout does not
        // RECEIVE a width: the pipeline derives its bounding box from the coordinates the layouts
        // actually place (Layouts::prepare, "max coordinate + 1 per axis"), so a width handed in
        // from outside would be a second, disagreeing source of truth. A script that wants one
        // declares it under its OWN name (`addUint8("cols", cols, 1, 64)`) and it becomes a real
        // slider. Not `width`: that is a system variable the engine writes, so a script cannot
        // declare it and the compiler refuses the name.
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = script_.engine().declaredControls(n);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t* slot = script_.engine().controlSlot(decls[i].offset);
            if (!slot) continue;
            // Published at the width the script declared. A uint16_t member reaches the UI as a
            // 16-bit control writing both its arena bytes; publishing it as a uint8 would drive
            // only the low one and leave the high half holding whatever it had.
            if (decls[i].type == moonlive::CtrlType::Uint16) {
                // Safe to view as a uint16_t: the compiler aligns every wide member to an even
                // arena offset (two backends cannot encode an odd halfword offset at all), and the
                // arena base comes from platform::alloc, which is aligned for any fundamental type.
                controls_.addUint16(decls[i].name, *reinterpret_cast<uint16_t*>(slot),
                                    decls[i].min, decls[i].max);
            } else {
                controls_.addUint8(decls[i].name, *slot,
                                   static_cast<uint8_t>(decls[i].min),
                                   static_cast<uint8_t>(decls[i].max));
            }
        }
    }

    /// Compile the script. The lights themselves are placed by whoever asks — see lightCount().
    void prepare() override {
        compile();
        rebuildControls();
    }

    /// Run the script, counting what it places.
    ///
    /// The Layer sizes its buffer from this before asking for a single coordinate, which is why it
    /// cannot come from the walk. Running the script twice is what every other layout does — the
    /// alternative is caching coordinates, and that is the allocation this design exists to avoid.
    nrOfLightsType lightCount() const override {
        compile();
        if (!script_.ok()) return 0;
        Counter c{0};
        runScript(&addToCounter, &c);
        return c.n;
    }

    /// Run the script again, emitting each light into the consumer's sink.
    void placeLights(const CoordSink& sink) const override {
        compile();
        if (!script_.ok()) return;
        Emitter e{&sink, 0};
        runScript(&addToSink, &e);
    }

    void release() override {
        script_.engine().free();
        script_.invalidate();     // forget what was compiled, so re-enabling rebuilds it
        LayoutBase::release();
    }

    /// Nothing to do on a control write, and that is the point.
    ///
    /// A control write lands DIRECTLY in the name buffer, so this override used to exist to clear a
    /// cached hash that the write would otherwise leave stale, keeping the previous script running
    /// under a new name. compile() now re-derives from the FILE every time, comparing its content
    /// hash, so a changed name and changed contents are both noticed without anything to clear.
    void onControlChanged(const char* name) override {
        // Nothing to invalidate: compile() re-derives from the FILE every time, comparing a content
        // hash, so a control write that lands directly in the name buffer is noticed on its own.
        (void)name;
    }

    /// Point the layout at a script in the shared script directory; the next prepare() compiles it.
    void setScript(const char* name) { script_.setName(name); }

private:
    /// Compile if the source has changed since the program that is loaded.
    ///
    /// Called from prepare(), and also from lightCount()/placeLights: because applyState() runs
    /// PARENT-FIRST (MoonModule.h): the container computes its bounding box by walking its children
    /// before those children have prepared. A layout whose count is arithmetic (GridLayout) does not
    /// notice; one that needs a compiled program would report an empty fixture to whoever asked
    /// first, and the pipeline would come up dark with no error anywhere.
    ///
    /// This const_cast is the ONE mechanism a scripted layout needs that a compiled one does not
    /// (architecture.md, MoonLive) — it exists only because of that prepare ordering. Removing it
    /// means letting children prepare before a container aggregates them, which is a core lifecycle
    /// change; until then the exception is here, named, rather than spread across the bindings.
    /// Single-threaded by construction: both the mapping rebuild that walks a layout and the tick
    /// that follows it run on the render thread, so the lazy compile below cannot overlap a walk.
    /// Moving layout work to a worker would change that — the engine would then need a published
    /// immutable program rather than one mutated in place.
    void compile() const {
        // sync() answers "is what is compiled still what the file says" from a content hash, so a
        // call that changes nothing costs a read rather than a re-JIT. That matters here more than
        // anywhere: this runs from lightCount() and placeLights() as well as prepare(), which the
        // Layer calls while it builds its mapping.
        auto* self = const_cast<MoonLiveLayout*>(this);
        self->script_.sync(moonlive::layoutSysVars(), *self);
    }

    struct Counter { nrOfLightsType n; };
    struct Emitter { const CoordSink* sink; nrOfLightsType idx; };

    static void addToCounter(void* ctx, uint16_t, uint16_t, uint16_t) {
        static_cast<Counter*>(ctx)->n++;
    }
    static void addToSink(void* ctx, uint16_t x, uint16_t y, uint16_t z) {
        auto* e = static_cast<Emitter*>(ctx);
        e->sink->pixel(e->idx++, static_cast<lengthType>(x),
                       static_cast<lengthType>(y), static_cast<lengthType>(z));
    }

    /// Point addLight at `fn` and run the script once.
    ///
    /// The engine writes through a buffer it is handed, but this script writes through addLight
    /// instead — so it is given a single scratch light, enough to satisfy run()'s "somewhere to
    /// write" precondition without staging anything. A script that also calls setXYZ scribbles
    /// there harmlessly.
    void runScript(moonlive::AddLightFn fn, void* ctx) const {
        uint8_t scratch[3] = {0, 0, 0};
        // Checked BEFORE the sink is installed. `ctx` is the caller's stack-local Counter or
        // Emitter, so returning between install and clear would leave the global sink pointing at
        // a dead frame until the next runScript happened to overwrite it.
        //
        // A script without placeLights places no lights, which the module reports as an empty
        // fixture rather than a failure.
        if (!script_.engine().hasEntry(moonlive::kEntryPlaceLights)) return;
        moonlive::setAddLightSink(fn, ctx);
        script_.engine().run(scratch, 1, 3, 0, moonlive::kEntryPlaceLights);
        moonlive::setAddLightSink(nullptr, nullptr);
    }

    // The script this layout places from. Empty on a fresh card: it reports "no script" and places
    // no lights until one is named, rather than every new layout compiling the same default grid.
    mutable moonlive::MoonLiveScript script_;

};

}  // namespace mm
